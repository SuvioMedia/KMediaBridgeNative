/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef KMEDIA_BRIDGE_TIMESTAMPS_H
#define KMEDIA_BRIDGE_TIMESTAMPS_H

#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>

/*
 * Matroska commonly carries presentation timestamps without decode timestamps.
 * Fragmented MP4 requires both for reordered video. This mirrors FFmpeg's
 * stream-copy timestamp estimation: begin before the first PTS by video_delay
 * frames, then advance in decode order at the stream frame rate.
 */
typedef struct KmbTimestampState {
    int initialized;
    int64_t next_dts_us;
} KmbTimestampState;

static inline int kmb_valid_rate(AVRational rate) {
    return rate.num > 0 && rate.den > 0;
}

/*
 * Timestamped playback restarts expose a source timeline that may begin many
 * minutes or hours after zero. CMAF consumers must not wait through that gap.
 * Keep the shared conversion in microseconds so audio and video use one
 * origin even when their stream time bases differ.
 */
static inline int kmb_timestamp_precedes_origin(
    int64_t timestamp,
    AVRational time_base,
    int64_t origin_us
) {
    if (timestamp == AV_NOPTS_VALUE || origin_us <= 0 || !kmb_valid_rate(time_base)) {
        return 0;
    }
    return av_rescale_q(timestamp, time_base, AV_TIME_BASE_Q) < origin_us;
}

static inline int64_t kmb_rebase_timestamp(
    int64_t timestamp,
    AVRational time_base,
    int64_t origin_us
) {
    int64_t relative_us = 0;
    if (timestamp == AV_NOPTS_VALUE || origin_us <= 0 || !kmb_valid_rate(time_base)) {
        return timestamp;
    }
    relative_us = av_rescale_q(timestamp, time_base, AV_TIME_BASE_Q) - origin_us;
    if (relative_us < 0) {
        relative_us = 0;
    }
    return av_rescale_q(relative_us, AV_TIME_BASE_Q, time_base);
}

static inline AVRational kmb_stream_frame_rate(const AVStream *stream) {
    if (kmb_valid_rate(stream->avg_frame_rate)) {
        return stream->avg_frame_rate;
    }
    if (stream->codecpar != NULL && kmb_valid_rate(stream->codecpar->framerate)) {
        return stream->codecpar->framerate;
    }
    return (AVRational){0, 1};
}

static inline int64_t kmb_packet_duration_us(const AVStream *stream, const AVPacket *packet) {
    const AVCodecParameters *parameters = stream->codecpar;
    if (packet->duration > 0) {
        return av_rescale_q(packet->duration, stream->time_base, AV_TIME_BASE_Q);
    }
    if (parameters != NULL && parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
        parameters->frame_size > 0 && parameters->sample_rate > 0) {
        return av_rescale_q(
            parameters->frame_size,
            (AVRational){1, parameters->sample_rate},
            AV_TIME_BASE_Q
        );
    }
    {
        const AVRational frame_rate = kmb_stream_frame_rate(stream);
        if (parameters != NULL && parameters->codec_type == AVMEDIA_TYPE_VIDEO &&
            kmb_valid_rate(frame_rate)) {
            return av_rescale_q(1, av_inv_q(frame_rate), AV_TIME_BASE_Q);
        }
    }
    return 0;
}

static inline void kmb_prepare_packet_timestamps(
    const AVStream *stream,
    AVPacket *packet,
    KmbTimestampState *state
) {
    const AVCodecParameters *parameters = stream->codecpar;
    const AVRational frame_rate = kmb_stream_frame_rate(stream);
    int64_t current_dts_us = AV_NOPTS_VALUE;

    if (packet->dts != AV_NOPTS_VALUE) {
        current_dts_us = av_rescale_q(packet->dts, stream->time_base, AV_TIME_BASE_Q);
    } else if (state->initialized) {
        current_dts_us = state->next_dts_us;
    } else {
        current_dts_us = 0;
        if (packet->pts != AV_NOPTS_VALUE) {
            current_dts_us = av_rescale_q(packet->pts, stream->time_base, AV_TIME_BASE_Q);
        }
        if (parameters != NULL && parameters->codec_type == AVMEDIA_TYPE_VIDEO &&
            parameters->video_delay > 0 && kmb_valid_rate(frame_rate)) {
            current_dts_us -= av_rescale_q(
                parameters->video_delay,
                av_inv_q(frame_rate),
                AV_TIME_BASE_Q
            );
        }
    }
    state->initialized = 1;

    if (packet->dts == AV_NOPTS_VALUE) {
        packet->dts = av_rescale_q(current_dts_us, AV_TIME_BASE_Q, stream->time_base);
    }
    if (packet->pts == AV_NOPTS_VALUE) {
        packet->pts = packet->dts;
    }

    if (parameters != NULL && parameters->codec_type == AVMEDIA_TYPE_VIDEO &&
        kmb_valid_rate(frame_rate)) {
        const int64_t decode_frame = av_rescale_q(
            current_dts_us,
            AV_TIME_BASE_Q,
            av_inv_q(frame_rate)
        );
        state->next_dts_us = av_rescale_q(
            decode_frame + 1,
            av_inv_q(frame_rate),
            AV_TIME_BASE_Q
        );
    } else {
        const int64_t duration_us = kmb_packet_duration_us(stream, packet);
        state->next_dts_us = current_dts_us + duration_us;
        if (packet->duration <= 0 && duration_us > 0) {
            packet->duration = av_rescale_q(duration_us, AV_TIME_BASE_Q, stream->time_base);
        }
    }
}

#endif
