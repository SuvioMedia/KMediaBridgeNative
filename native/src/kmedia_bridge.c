/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "kmedia_bridge.h"
#include "kmedia_bridge_timestamps.h"

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/bprint.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

static void kmb_set_error(char **output_error, const char *message) {
    if (output_error == NULL) {
        return;
    }
    *output_error = av_strdup(message != NULL ? message : "Unknown media bridge error.");
}

static void kmb_set_av_error(char **output_error, const char *operation, int error_code) {
    char error_text[AV_ERROR_MAX_STRING_SIZE] = {0};
    char combined[256] = {0};
    av_strerror(error_code, error_text, sizeof(error_text));
    av_strlcpy(combined, operation, sizeof(combined));
    av_strlcat(combined, ": ", sizeof(combined));
    av_strlcat(combined, error_text, sizeof(combined));
    kmb_set_error(output_error, combined);
}

static void kmb_json_string(AVBPrint *output, const char *value) {
    const unsigned char *cursor = (const unsigned char *)(value != NULL ? value : "unknown");
    av_bprint_chars(output, '"', 1);
    while (*cursor != '\0') {
        switch (*cursor) {
            case '"':
                av_bprintf(output, "\\\"");
                break;
            case '\\':
                av_bprintf(output, "\\\\");
                break;
            case '\b':
                av_bprintf(output, "\\b");
                break;
            case '\f':
                av_bprintf(output, "\\f");
                break;
            case '\n':
                av_bprintf(output, "\\n");
                break;
            case '\r':
                av_bprintf(output, "\\r");
                break;
            case '\t':
                av_bprintf(output, "\\t");
                break;
            default:
                if (*cursor < 0x20) {
                    av_bprintf(output, "\\u%04x", *cursor);
                } else {
                    av_bprint_chars(output, (char)*cursor, 1);
                }
                break;
        }
        cursor++;
    }
    av_bprint_chars(output, '"', 1);
}

static const char *kmb_dynamic_range(const AVCodecParameters *parameters) {
    const AVPacketSideData *dolby_vision = av_packet_side_data_get(
        parameters->coded_side_data,
        parameters->nb_coded_side_data,
        AV_PKT_DATA_DOVI_CONF
    );
    const AVPacketSideData *hdr10_plus = av_packet_side_data_get(
        parameters->coded_side_data,
        parameters->nb_coded_side_data,
        AV_PKT_DATA_DYNAMIC_HDR10_PLUS
    );
    if (dolby_vision != NULL) {
        return "DOLBY_VISION";
    }
    if (hdr10_plus != NULL) {
        return "HDR10_PLUS";
    }
    if (parameters->color_trc == AVCOL_TRC_SMPTE2084) {
        return "HDR10";
    }
    if (parameters->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        return "HLG";
    }
    if (parameters->color_trc == AVCOL_TRC_BT709 || parameters->color_trc == AVCOL_TRC_IEC61966_2_1) {
        return "SDR";
    }
    return "UNKNOWN";
}

static int kmb_bit_depth(const AVCodecParameters *parameters) {
    const AVPixFmtDescriptor *descriptor = NULL;
    if (parameters->bits_per_raw_sample > 0) {
        return parameters->bits_per_raw_sample;
    }
    if (parameters->format >= 0) {
        descriptor = av_pix_fmt_desc_get((enum AVPixelFormat)parameters->format);
    }
    return descriptor != NULL ? descriptor->comp[0].depth : 0;
}

static const char *kmb_metadata_value(const AVStream *stream, const char *key) {
    const AVDictionaryEntry *entry = av_dict_get(stream->metadata, key, NULL, 0);
    return entry != NULL && entry->value != NULL && entry->value[0] != '\0' ? entry->value : NULL;
}

static void kmb_append_track_metadata(AVBPrint *output, const AVStream *stream) {
    const char *language = kmb_metadata_value(stream, "language");
    const char *title = kmb_metadata_value(stream, "title");
    av_bprintf(output, ",\"language\":");
    if (language == NULL) {
        av_bprintf(output, "null");
    } else {
        kmb_json_string(output, language);
    }
    av_bprintf(output, ",\"title\":");
    if (title == NULL) {
        av_bprintf(output, "null");
    } else {
        kmb_json_string(output, title);
    }
    av_bprintf(
        output,
        ",\"isDefault\":%s",
        (stream->disposition & AV_DISPOSITION_DEFAULT) != 0 ? "true" : "false"
    );
}

static void kmb_append_video_track(AVBPrint *output, const AVStream *stream) {
    const AVCodecParameters *parameters = stream->codecpar;
    const AVPacketSideData *mastering_display = av_packet_side_data_get(
        parameters->coded_side_data,
        parameters->nb_coded_side_data,
        AV_PKT_DATA_MASTERING_DISPLAY_METADATA
    );
    const AVPacketSideData *content_light = av_packet_side_data_get(
        parameters->coded_side_data,
        parameters->nb_coded_side_data,
        AV_PKT_DATA_CONTENT_LIGHT_LEVEL
    );
    const AVPacketSideData *hdr10_plus = av_packet_side_data_get(
        parameters->coded_side_data,
        parameters->nb_coded_side_data,
        AV_PKT_DATA_DYNAMIC_HDR10_PLUS
    );
    const AVPacketSideData *dolby_vision = av_packet_side_data_get(
        parameters->coded_side_data,
        parameters->nb_coded_side_data,
        AV_PKT_DATA_DOVI_CONF
    );
    const AVDOVIDecoderConfigurationRecord *dovi =
        dolby_vision != NULL && dolby_vision->size >= sizeof(AVDOVIDecoderConfigurationRecord)
            ? (const AVDOVIDecoderConfigurationRecord *)dolby_vision->data
            : NULL;
    const AVMasteringDisplayMetadata *mastering =
        mastering_display != NULL && mastering_display->size >= sizeof(AVMasteringDisplayMetadata)
            ? (const AVMasteringDisplayMetadata *)mastering_display->data
            : NULL;
    const AVContentLightMetadata *light =
        content_light != NULL && content_light->size >= sizeof(AVContentLightMetadata)
            ? (const AVContentLightMetadata *)content_light->data
            : NULL;
    const AVRational frame_rate = stream->avg_frame_rate;

    av_bprintf(output, "{\"type\":\"video\",\"id\":%d,\"codec\":", stream->index);
    kmb_json_string(output, avcodec_get_name(parameters->codec_id));
    av_bprintf(
        output,
        ",\"profile\":%d,\"level\":%d,\"width\":%d,\"height\":%d,\"bitDepth\":%d",
        parameters->profile,
        parameters->level,
        parameters->width,
        parameters->height,
        kmb_bit_depth(parameters)
    );
    av_bprintf(output, ",\"dynamicRange\":");
    kmb_json_string(output, kmb_dynamic_range(parameters));
    av_bprintf(output, ",\"colorRange\":");
    kmb_json_string(output, av_color_range_name(parameters->color_range));
    av_bprintf(output, ",\"colorPrimaries\":");
    kmb_json_string(output, av_color_primaries_name(parameters->color_primaries));
    av_bprintf(output, ",\"colorTransfer\":");
    kmb_json_string(output, av_color_transfer_name(parameters->color_trc));
    av_bprintf(output, ",\"colorMatrix\":");
    kmb_json_string(output, av_color_space_name(parameters->color_space));
    av_bprintf(
        output,
        ",\"frameRateNumerator\":%d,\"frameRateDenominator\":%d,\"hasHdr10PlusMetadata\":%s",
        frame_rate.num,
        frame_rate.den,
        hdr10_plus != NULL ? "true" : "false"
    );
    if (dovi != NULL) {
        av_bprintf(
            output,
            ",\"dolbyVision\":{\"profile\":%u,\"level\":%u,\"hasRpu\":%s,\"hasEnhancementLayer\":%s}",
            dovi->dv_profile,
            dovi->dv_level,
            dovi->rpu_present_flag ? "true" : "false",
            dovi->el_present_flag ? "true" : "false"
        );
    } else {
        av_bprintf(output, ",\"dolbyVision\":null");
    }
    if (mastering != NULL && mastering->has_primaries && mastering->has_luminance) {
        av_bprintf(
            output,
            ",\"masteringDisplay\":{"
            "\"redX\":%.10g,\"redY\":%.10g,"
            "\"greenX\":%.10g,\"greenY\":%.10g,"
            "\"blueX\":%.10g,\"blueY\":%.10g,"
            "\"whiteX\":%.10g,\"whiteY\":%.10g,"
            "\"minimumLuminanceNits\":%.10g,\"maximumLuminanceNits\":%.10g}",
            av_q2d(mastering->display_primaries[0][0]),
            av_q2d(mastering->display_primaries[0][1]),
            av_q2d(mastering->display_primaries[1][0]),
            av_q2d(mastering->display_primaries[1][1]),
            av_q2d(mastering->display_primaries[2][0]),
            av_q2d(mastering->display_primaries[2][1]),
            av_q2d(mastering->white_point[0]),
            av_q2d(mastering->white_point[1]),
            av_q2d(mastering->min_luminance),
            av_q2d(mastering->max_luminance)
        );
    } else {
        av_bprintf(output, ",\"masteringDisplay\":null");
    }
    if (light != NULL) {
        av_bprintf(
            output,
            ",\"contentLightLevel\":{"
            "\"maximumContentLightLevelNits\":%u,"
            "\"maximumFrameAverageLightLevelNits\":%u}",
            light->MaxCLL,
            light->MaxFALL
        );
    } else {
        av_bprintf(output, ",\"contentLightLevel\":null");
    }
    kmb_append_track_metadata(output, stream);
    av_bprintf(output, "}");
}

static void kmb_append_audio_track(AVBPrint *output, const AVStream *stream) {
    const AVCodecParameters *parameters = stream->codecpar;
    av_bprintf(output, "{\"type\":\"audio\",\"id\":%d,\"codec\":", stream->index);
    kmb_json_string(output, avcodec_get_name(parameters->codec_id));
    av_bprintf(
        output,
        ",\"channels\":%d,\"sampleRateHz\":%d,\"bitrate\":%lld",
        parameters->ch_layout.nb_channels,
        parameters->sample_rate,
        (long long)parameters->bit_rate
    );
    kmb_append_track_metadata(output, stream);
    av_bprintf(output, "}");
}

static void kmb_append_subtitle_track(AVBPrint *output, const AVStream *stream) {
    const AVCodecParameters *parameters = stream->codecpar;
    const AVCodecDescriptor *descriptor = avcodec_descriptor_get(parameters->codec_id);
    av_bprintf(output, "{\"type\":\"subtitle\",\"id\":%d,\"codec\":", stream->index);
    kmb_json_string(output, avcodec_get_name(parameters->codec_id));
    av_bprintf(
        output,
        ",\"isImageBased\":%s",
        descriptor != NULL && (descriptor->props & AV_CODEC_PROP_BITMAP_SUB) != 0 ? "true" : "false"
    );
    kmb_append_track_metadata(output, stream);
    av_bprintf(output, "}");
}

uint32_t kmb_abi_version(void) {
    return KMB_ABI_VERSION;
}

const char *kmb_ffmpeg_version(void) {
    return av_version_info();
}

const char *kmb_ffmpeg_license(void) {
    return avutil_license();
}

const char *kmb_ffmpeg_configuration(void) {
    return avformat_configuration();
}

const char *kmb_runtime_features_json(void) {
#if defined(KMB_ENABLE_AVFOUNDATION_TRANSCODE)
    return "{\"subtitleBurnIn\":true,\"hdrToSdrToneMap\":true,\"avFoundationCompatibility\":true}";
#elif defined(KMB_ENABLE_SUBTITLE_BURN_IN) && defined(KMB_ENABLE_HDR_TO_SDR)
    return "{\"subtitleBurnIn\":true,\"hdrToSdrToneMap\":true}";
#elif defined(KMB_ENABLE_SUBTITLE_BURN_IN)
    return "{\"subtitleBurnIn\":true,\"hdrToSdrToneMap\":false}";
#elif defined(KMB_ENABLE_HDR_TO_SDR)
    return "{\"subtitleBurnIn\":false,\"hdrToSdrToneMap\":true}";
#else
    return "{\"subtitleBurnIn\":false,\"hdrToSdrToneMap\":false}";
#endif
}

KmbResult kmb_probe_json(const char *input_locator, char **output_json, char **output_error) {
    AVFormatContext *input = NULL;
    AVBPrint json;
    unsigned int index = 0;
    int appended = 0;
    int result = 0;

    if (output_json != NULL) {
        *output_json = NULL;
    }
    if (output_error != NULL) {
        *output_error = NULL;
    }
    if (input_locator == NULL || output_json == NULL) {
        kmb_set_error(output_error, "Input and output pointers are required.");
        return KMB_INVALID_ARGUMENT;
    }

    result = avformat_open_input(&input, input_locator, NULL, NULL);
    if (result < 0) {
        kmb_set_av_error(output_error, "Could not open media input", result);
        return KMB_OPEN_INPUT_FAILED;
    }
    result = avformat_find_stream_info(input, NULL);
    if (result < 0) {
        kmb_set_av_error(output_error, "Could not read media stream information", result);
        avformat_close_input(&input);
        return KMB_STREAM_INFO_FAILED;
    }

    av_bprint_init(&json, 1024, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&json, "{\"format\":");
    kmb_json_string(&json, input->iformat != NULL ? input->iformat->name : "unknown");
    if (input->duration == AV_NOPTS_VALUE) {
        av_bprintf(&json, ",\"durationUs\":null,\"tracks\":[");
    } else {
        av_bprintf(&json, ",\"durationUs\":%lld,\"tracks\":[", (long long)input->duration);
    }

    for (index = 0; index < input->nb_streams; index++) {
        const AVStream *stream = input->streams[index];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            continue;
        }
        if (appended) {
            av_bprint_chars(&json, ',', 1);
        }
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            kmb_append_video_track(&json, stream);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            kmb_append_audio_track(&json, stream);
        } else {
            kmb_append_subtitle_track(&json, stream);
        }
        appended = 1;
    }
    av_bprintf(&json, "]}");

    result = av_bprint_finalize(&json, output_json);
    avformat_close_input(&input);
    if (result < 0 || *output_json == NULL) {
        kmb_set_error(output_error, "Could not allocate the probe result.");
        return KMB_ALLOCATION_FAILED;
    }
    return KMB_OK;
}

typedef struct KmbWriteState {
    KmbWriteCallback callback;
    void *opaque;
    int cancelled;
} KmbWriteState;

static int kmb_select_track(const AVFormatContext *input, enum AVMediaType type, int requested_track_id) {
    int first = -1;
    unsigned int index = 0;
    if (requested_track_id >= 0) {
        if ((unsigned int)requested_track_id >= input->nb_streams ||
            input->streams[requested_track_id]->codecpar->codec_type != type) {
            return -2;
        }
        return requested_track_id;
    }
    for (index = 0; index < input->nb_streams; index++) {
        const AVStream *stream = input->streams[index];
        if (stream->codecpar->codec_type != type) {
            continue;
        }
        if (first < 0) {
            first = (int)index;
        }
        if ((stream->disposition & AV_DISPOSITION_DEFAULT) != 0) {
            return (int)index;
        }
    }
    return first;
}

static int kmb_write_packet(void *opaque, const uint8_t *bytes, int size) {
    KmbWriteState *state = (KmbWriteState *)opaque;
    if (state == NULL || state->callback == NULL || size < 0) {
        return AVERROR(EINVAL);
    }
    if (state->callback(state->opaque, bytes, size) != 0) {
        state->cancelled = 1;
        return AVERROR_EXIT;
    }
    return size;
}

static KmbResult kmb_remux_fragmented_mp4_internal(
    const char *input_locator,
    const char *output_path,
    int64_t fragment_duration_us,
    int64_t start_time_us,
    int preferred_video_track_id,
    int preferred_audio_track_id,
    KmbWriteCallback write_callback,
    void *opaque,
    char **output_error
) {
    AVFormatContext *input = NULL;
    AVFormatContext *output = NULL;
    AVPacket *packet = NULL;
    AVIOContext *custom_io = NULL;
    unsigned char *custom_buffer = NULL;
    AVDictionary *muxer_options = NULL;
    int *stream_mapping = NULL;
    KmbTimestampState *timestamp_states = NULL;
    int output_stream_count = 0;
    int selected_video_track_id = -1;
    int selected_audio_track_id = -1;
    int result = 0;
    unsigned int index = 0;
    KmbResult bridge_result = KMB_OK;
    KmbWriteState write_state = {write_callback, opaque, 0};
    const int uses_callback = write_callback != NULL;

    if (output_error != NULL) {
        *output_error = NULL;
    }
    if (input_locator == NULL || fragment_duration_us <= 0 || start_time_us < 0 ||
        (!uses_callback && output_path == NULL)) {
        kmb_set_error(output_error, "Valid input, output, fragment duration, and start time are required.");
        return KMB_INVALID_ARGUMENT;
    }

    result = avformat_open_input(&input, input_locator, NULL, NULL);
    if (result < 0) {
        kmb_set_av_error(output_error, "Could not open media input", result);
        return KMB_OPEN_INPUT_FAILED;
    }
    result = avformat_find_stream_info(input, NULL);
    if (result < 0) {
        kmb_set_av_error(output_error, "Could not read media stream information", result);
        bridge_result = KMB_STREAM_INFO_FAILED;
        goto cleanup;
    }
    selected_video_track_id = kmb_select_track(input, AVMEDIA_TYPE_VIDEO, preferred_video_track_id);
    if (selected_video_track_id < 0) {
        kmb_set_error(
            output_error,
            selected_video_track_id == -2 ? "The requested video track does not exist." : "No video track is available."
        );
        bridge_result = KMB_UNSUPPORTED;
        goto cleanup;
    }
    if (preferred_audio_track_id != -2) {
        selected_audio_track_id = kmb_select_track(input, AVMEDIA_TYPE_AUDIO, preferred_audio_track_id);
        if (selected_audio_track_id == -2) {
            kmb_set_error(output_error, "The requested audio track does not exist.");
            bridge_result = KMB_UNSUPPORTED;
            goto cleanup;
        }
    }
    if (start_time_us > 0) {
        result = avformat_seek_file(input, -1, INT64_MIN, start_time_us, start_time_us, AVSEEK_FLAG_BACKWARD);
        if (result < 0) {
            kmb_set_av_error(output_error, "Could not seek media input", result);
            bridge_result = KMB_READ_FAILED;
            goto cleanup;
        }
        avformat_flush(input);
    }
    result = avformat_alloc_output_context2(&output, NULL, "mp4", output_path);
    if (result < 0 || output == NULL) {
        kmb_set_av_error(output_error, "Could not create fragmented MP4 output", result);
        bridge_result = KMB_OPEN_OUTPUT_FAILED;
        goto cleanup;
    }

    stream_mapping = av_calloc(input->nb_streams, sizeof(*stream_mapping));
    timestamp_states = av_calloc(input->nb_streams, sizeof(*timestamp_states));
    if (stream_mapping == NULL || timestamp_states == NULL) {
        kmb_set_error(output_error, "Could not allocate stream mapping and timestamp state.");
        bridge_result = KMB_ALLOCATION_FAILED;
        goto cleanup;
    }
    for (index = 0; index < input->nb_streams; index++) {
        const AVStream *input_stream = input->streams[index];
        AVStream *output_stream = NULL;
        stream_mapping[index] = -1;
        if ((int)index != selected_video_track_id && (int)index != selected_audio_track_id) {
            continue;
        }
        if (avformat_query_codec(output->oformat, input_stream->codecpar->codec_id, FF_COMPLIANCE_NORMAL) <= 0) {
            kmb_set_error(output_error, "A selected media track cannot be represented in fragmented MP4.");
            bridge_result = KMB_UNSUPPORTED;
            goto cleanup;
        }
        output_stream = avformat_new_stream(output, NULL);
        if (output_stream == NULL) {
            kmb_set_error(output_error, "Could not create an output stream.");
            bridge_result = KMB_ALLOCATION_FAILED;
            goto cleanup;
        }
        stream_mapping[index] = output_stream_count++;
        result = avcodec_parameters_copy(output_stream->codecpar, input_stream->codecpar);
        if (result < 0) {
            kmb_set_av_error(output_error, "Could not copy stream parameters", result);
            bridge_result = KMB_WRITE_FAILED;
            goto cleanup;
        }
        output_stream->codecpar->codec_tag = 0;
        if (output_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            output_stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }
        output_stream->time_base = input_stream->time_base;
    }

    if (uses_callback) {
        custom_buffer = av_malloc(32 * 1024);
        if (custom_buffer == NULL) {
            kmb_set_error(output_error, "Could not allocate the output callback buffer.");
            bridge_result = KMB_ALLOCATION_FAILED;
            goto cleanup;
        }
        custom_io = avio_alloc_context(custom_buffer, 32 * 1024, 1, &write_state, NULL, kmb_write_packet, NULL);
        if (custom_io == NULL) {
            kmb_set_error(output_error, "Could not create the output callback context.");
            bridge_result = KMB_ALLOCATION_FAILED;
            goto cleanup;
        }
        custom_buffer = NULL;
        output->pb = custom_io;
        output->flags |= AVFMT_FLAG_CUSTOM_IO;
    } else if (!(output->oformat->flags & AVFMT_NOFILE)) {
        result = avio_open(&output->pb, output_path, AVIO_FLAG_WRITE);
        if (result < 0) {
            kmb_set_av_error(output_error, "Could not open the output path", result);
            bridge_result = KMB_OPEN_OUTPUT_FAILED;
            goto cleanup;
        }
    }

    av_dict_set(
        &muxer_options,
        "movflags",
        "frag_keyframe+delay_moov+default_base_moof+negative_cts_offsets",
        0
    );
    av_dict_set_int(&muxer_options, "frag_duration", fragment_duration_us, 0);
    result = avformat_write_header(output, &muxer_options);
    if (result < 0) {
        if (write_state.cancelled) {
            bridge_result = KMB_CANCELLED;
            goto cleanup;
        }
        kmb_set_av_error(output_error, "Could not write the fragmented MP4 header", result);
        bridge_result = KMB_WRITE_FAILED;
        goto cleanup;
    }

    packet = av_packet_alloc();
    if (packet == NULL) {
        kmb_set_error(output_error, "Could not allocate a media packet.");
        bridge_result = KMB_ALLOCATION_FAILED;
        goto cleanup;
    }

    while ((result = av_read_frame(input, packet)) >= 0) {
        AVStream *input_stream = NULL;
        AVStream *output_stream = NULL;
        const int mapped_index = stream_mapping[packet->stream_index];
        if (mapped_index < 0) {
            av_packet_unref(packet);
            continue;
        }
        input_stream = input->streams[packet->stream_index];
        output_stream = output->streams[mapped_index];
        kmb_prepare_packet_timestamps(
            input_stream,
            packet,
            &timestamp_states[packet->stream_index]
        );
        packet->stream_index = mapped_index;
        av_packet_rescale_ts(packet, input_stream->time_base, output_stream->time_base);
        packet->pos = -1;
        result = av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
        if (result < 0) {
            if (write_state.cancelled) {
                bridge_result = KMB_CANCELLED;
                goto cleanup;
            }
            kmb_set_av_error(output_error, "Could not write a media packet", result);
            bridge_result = KMB_WRITE_FAILED;
            goto cleanup;
        }
    }
    if (result != AVERROR_EOF) {
        kmb_set_av_error(output_error, "Could not read a media packet", result);
        bridge_result = KMB_READ_FAILED;
        goto cleanup;
    }
    result = av_write_trailer(output);
    if (result < 0) {
        if (write_state.cancelled) {
            bridge_result = KMB_CANCELLED;
        } else {
            kmb_set_av_error(output_error, "Could not finalize fragmented MP4 output", result);
            bridge_result = KMB_WRITE_FAILED;
        }
    }

cleanup:
    av_dict_free(&muxer_options);
    av_packet_free(&packet);
    av_freep(&stream_mapping);
    av_freep(&timestamp_states);
    if (custom_io != NULL) {
        if (output != NULL) {
            output->pb = NULL;
        }
        avio_context_free(&custom_io);
    } else if (output != NULL && !(output->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&output->pb);
    }
    av_freep(&custom_buffer);
    avformat_free_context(output);
    avformat_close_input(&input);
    return bridge_result;
}

KmbResult kmb_remux_fragmented_mp4(
    const char *input_locator,
    const char *output_path,
    char **output_error
) {
    return kmb_remux_fragmented_mp4_internal(
        input_locator,
        output_path,
        4000000,
        0,
        -1,
        -1,
        NULL,
        NULL,
        output_error
    );
}

KmbResult kmb_remux_fragmented_mp4_stream(
    const char *input_locator,
    int64_t fragment_duration_us,
    int64_t start_time_us,
    int32_t preferred_video_track_id,
    int32_t preferred_audio_track_id,
    KmbWriteCallback write_callback,
    void *opaque,
    char **output_error
) {
    if (write_callback == NULL) {
        kmb_set_error(output_error, "An output callback is required.");
        return KMB_INVALID_ARGUMENT;
    }
    return kmb_remux_fragmented_mp4_internal(
        input_locator,
        NULL,
        fragment_duration_us,
        start_time_us,
        preferred_video_track_id,
        preferred_audio_track_id,
        write_callback,
        opaque,
        output_error
    );
}

void kmb_free_string(char *value) {
    av_free(value);
}
