/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "kmedia_bridge_hevc.h"

#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <string.h>

#define KMB_HEVC_PREFIX_BYTES (32U * 1024U * 1024U)
#define KMB_HEVC_CONFIGURATION_BYTES (1024U * 1024U)

int kmb_hevc_configuration_complete(const uint8_t *data, size_t size) {
    if (data == NULL || size < 23 || data[0] != 1) return 0;
    unsigned int types = 0;
    size_t position = 23;
    for (unsigned int array = 0; array < data[22]; array++) {
        if (size - position < 3) return 0;
        unsigned int type = data[position] & 63;
        unsigned int count = AV_RB16(data + position + 1);
        position += 3;
        for (unsigned int nal = 0; nal < count; nal++) {
            if (size - position < 2) return 0;
            size_t length = AV_RB16(data + position);
            position += 2;
            if (length < 2 || length > size - position || ((data[position] >> 1) & 63) != type) return 0;
            if (type >= 32 && type <= 34) types |= 1U << (type - 32);
            position += length;
        }
    }
    return position == size && types == 7;
}

static int annex_b_configuration_complete(const uint8_t *data, size_t size) {
    unsigned int types = 0;
    if (size > KMB_HEVC_CONFIGURATION_BYTES) return 0;
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            unsigned int type = (data[i + 3] >> 1) & 63;
            if (type >= 32 && type <= 34) types |= 1U << (type - 32);
        }
    }
    return types == 7;
}

static int filter_packet(KmbHevcPreparation *state, AVPacket *packet) {
    int result = av_bsf_send_packet(state->filter, packet);
    if (result < 0) return result;
    return av_bsf_receive_packet(state->filter, packet);
}

/* hev1 may carry all VPS/SPS/PPS in its samples and have an empty hvcC array.
 * Recover the configuration before writing hvc1's initialization segment. The
 * FFmpeg filters preserve compressed pictures and convert their NAL framing to
 * match the recovered Annex B extradata; the MP4 muxer writes both back as hvc1.
 * Keep the scanned packets so preparation works without seeking or losing audio.
 */
int kmb_hevc_prepare(AVFormatContext *input, int video_track, KmbHevcPreparation *state) {
    AVStream *video = input->streams[video_track];
    AVCodecParameters *parameters = video->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_HEVC || parameters->extradata == NULL || parameters->extradata_size < 23 ||
        parameters->extradata[0] != 1 ||
        kmb_hevc_configuration_complete(parameters->extradata, (size_t)parameters->extradata_size)) {
        return 0;
    }
    int result = av_bsf_list_parse_str("hevc_mp4toannexb,extract_extradata", &state->filter);
    if (result < 0) return result;
    result = avcodec_parameters_copy(state->filter->par_in, parameters);
    if (result < 0) return result;
    state->filter->time_base_in = video->time_base;
    result = av_bsf_init(state->filter);
    if (result < 0) return result;

    size_t buffered_bytes = 0;
    unsigned int scanned_packets = 0;
    while (scanned_packets++ < KMB_HEVC_PREFIX_PACKETS && buffered_bytes < KMB_HEVC_PREFIX_BYTES) {
        AVPacket *packet = av_packet_alloc();
        if (packet == NULL) return AVERROR(ENOMEM);
        result = av_read_frame(input, packet);
        if (result < 0) {
            av_packet_free(&packet);
            return result;
        }
        if (packet->stream_index == video_track) result = filter_packet(state, packet);
        if (result < 0) {
            av_packet_free(&packet);
            if (result == AVERROR(EAGAIN)) continue;
            return result;
        }
        if (packet->size < 0 || (size_t)packet->size > KMB_HEVC_PREFIX_BYTES - buffered_bytes) {
            av_packet_free(&packet);
            return AVERROR_INVALIDDATA;
        }
        buffered_bytes += (size_t)packet->size;
        state->prefix[state->count++] = packet;
        if (packet->stream_index != video_track) continue;
        size_t size = 0;
        const uint8_t *configuration = av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, &size);
        if (configuration == NULL || !annex_b_configuration_complete(configuration, size)) continue;
        uint8_t *copy = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (copy == NULL) return AVERROR(ENOMEM);
        memcpy(copy, configuration, size);
        av_freep(&parameters->extradata);
        parameters->extradata = copy;
        parameters->extradata_size = (int)size;
        return 0;
    }
    return AVERROR_INVALIDDATA;
}

int kmb_hevc_read_frame(AVFormatContext *input, int video_track, KmbHevcPreparation *state, AVPacket *packet) {
    if (state->next < state->count) {
        AVPacket **buffered = &state->prefix[state->next++];
        av_packet_move_ref(packet, *buffered);
        av_packet_free(buffered);
        return 0;
    }
    for (;;) {
        int result = av_read_frame(input, packet);
        if (result < 0 || state->filter == NULL || packet->stream_index != video_track) return result;
        result = filter_packet(state, packet);
        if (result != AVERROR(EAGAIN)) return result;
    }
}

void kmb_hevc_discard_prefix(KmbHevcPreparation *state) {
    while (state->next < state->count) av_packet_free(&state->prefix[state->next++]);
    state->count = state->next = 0;
    if (state->filter != NULL) av_bsf_flush(state->filter);
}

void kmb_hevc_close(KmbHevcPreparation *state) {
    kmb_hevc_discard_prefix(state);
    av_bsf_free(&state->filter);
}
