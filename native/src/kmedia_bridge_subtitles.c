/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "kmedia_bridge.h"
#include "kmedia_bridge_timestamps.h"

#include <libavutil/avstring.h>
#include <libavutil/mem.h>

#if defined(KMB_ENABLE_SUBTITLE_BURN_IN)

#include "kmedia_bridge_avc_encoder.h"

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

#define KMB_CODEC_EAGAIN_RETRY_LIMIT 5000
#define KMB_CODEC_EAGAIN_RETRY_DELAY_US 1000

typedef struct KmbSubtitleWriteState {
    KmbWriteCallback callback;
    void *opaque;
    int cancelled;
} KmbSubtitleWriteState;

typedef struct KmbSubtitlePipeline {
    AVFormatContext *input;
    AVFormatContext *output;
    AVCodecContext *decoder;
    AVCodecContext *encoder;
    AVFilterGraph *filter_graph;
    AVFilterContext *buffer_source;
    AVFilterContext *buffer_sink;
    AVPacket *input_packet;
    AVPacket *encoded_packet;
    AVFrame *decoded_frame;
    AVFrame *filtered_frame;
    AVIOContext *custom_io;
    unsigned char *custom_buffer;
    AVDictionary *muxer_options;
    int selected_video_track_id;
    int selected_audio_track_id;
    int selected_subtitle_track_id;
    int output_video_track_id;
    int output_audio_track_id;
    KmbTimestampState audio_timestamp_state;
    KmbSubtitleWriteState write_state;
} KmbSubtitlePipeline;

static void kmb_subtitle_set_error(char **output_error, const char *message) {
    if (output_error != NULL) {
        *output_error = av_strdup(message != NULL ? message : "Unknown subtitle pipeline error.");
    }
}

static void kmb_subtitle_set_av_error(char **output_error, const char *operation, int error_code) {
    char error_text[AV_ERROR_MAX_STRING_SIZE] = {0};
    char combined[256] = {0};
    av_strerror(error_code, error_text, sizeof(error_text));
    av_strlcpy(combined, operation, sizeof(combined));
    av_strlcat(combined, ": ", sizeof(combined));
    av_strlcat(combined, error_text, sizeof(combined));
    kmb_subtitle_set_error(output_error, combined);
}

static int kmb_subtitle_select_track(
    const AVFormatContext *input,
    enum AVMediaType type,
    int requested_track_id
) {
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

static int kmb_subtitle_ordinal(const AVFormatContext *input, int absolute_track_id) {
    int ordinal = 0;
    unsigned int index = 0;
    for (index = 0; index < input->nb_streams; index++) {
        if (input->streams[index]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            continue;
        }
        if ((int)index == absolute_track_id) {
            return ordinal;
        }
        ordinal++;
    }
    return -1;
}

static int kmb_subtitle_is_hdr(const AVCodecParameters *parameters) {
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
    return dolby_vision != NULL || hdr10_plus != NULL ||
        parameters->color_trc == AVCOL_TRC_SMPTE2084 ||
        parameters->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
        parameters->color_primaries == AVCOL_PRI_BT2020 ||
        parameters->color_space == AVCOL_SPC_BT2020_NCL ||
        parameters->color_space == AVCOL_SPC_BT2020_CL;
}

static enum AVColorSpace kmb_subtitle_sdr_colorspace(const AVCodecParameters *parameters) {
    switch (parameters->color_space) {
        case AVCOL_SPC_BT709:
        case AVCOL_SPC_FCC:
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_SMPTE240M:
            return parameters->color_space;
        default:
            return parameters->height > 0 && parameters->height <= 576
                ? AVCOL_SPC_SMPTE170M
                : AVCOL_SPC_BT709;
    }
}

static enum AVColorRange kmb_subtitle_sdr_range(const AVCodecParameters *parameters) {
    return parameters->color_range == AVCOL_RANGE_JPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

static int kmb_subtitle_write_packet(void *opaque, const uint8_t *bytes, int size) {
    KmbSubtitleWriteState *state = (KmbSubtitleWriteState *)opaque;
    if (state == NULL || state->callback == NULL || size < 0) {
        return AVERROR(EINVAL);
    }
    if (state->callback(state->opaque, bytes, size) != 0) {
        state->cancelled = 1;
        return AVERROR_EXIT;
    }
    return size;
}

static void kmb_subtitle_cleanup(KmbSubtitlePipeline *pipeline) {
    av_dict_free(&pipeline->muxer_options);
    av_packet_free(&pipeline->input_packet);
    av_packet_free(&pipeline->encoded_packet);
    av_frame_free(&pipeline->decoded_frame);
    av_frame_free(&pipeline->filtered_frame);
    avfilter_graph_free(&pipeline->filter_graph);
    avcodec_free_context(&pipeline->decoder);
    avcodec_free_context(&pipeline->encoder);
    if (pipeline->custom_io != NULL) {
        if (pipeline->output != NULL) {
            pipeline->output->pb = NULL;
        }
        avio_context_free(&pipeline->custom_io);
    }
    av_freep(&pipeline->custom_buffer);
    avformat_free_context(pipeline->output);
    avformat_close_input(&pipeline->input);
}

static KmbResult kmb_subtitle_open_input(
    KmbSubtitlePipeline *pipeline,
    const char *input_locator,
    int preferred_video_track_id,
    int preferred_audio_track_id,
    int preferred_subtitle_track_id,
    int64_t start_time_us,
    char **output_error
) {
    int result = avformat_open_input(&pipeline->input, input_locator, NULL, NULL);
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not open media input", result);
        return KMB_OPEN_INPUT_FAILED;
    }
    result = avformat_find_stream_info(pipeline->input, NULL);
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not read media stream information", result);
        return KMB_STREAM_INFO_FAILED;
    }
    pipeline->selected_video_track_id =
        kmb_subtitle_select_track(pipeline->input, AVMEDIA_TYPE_VIDEO, preferred_video_track_id);
    pipeline->selected_subtitle_track_id =
        kmb_subtitle_select_track(pipeline->input, AVMEDIA_TYPE_SUBTITLE, preferred_subtitle_track_id);
    pipeline->selected_audio_track_id = -1;
    if (preferred_audio_track_id != -2) {
        pipeline->selected_audio_track_id =
            kmb_subtitle_select_track(pipeline->input, AVMEDIA_TYPE_AUDIO, preferred_audio_track_id);
    }
    if (pipeline->selected_video_track_id < 0 || pipeline->selected_subtitle_track_id < 0 ||
        pipeline->selected_audio_track_id == -2) {
        kmb_subtitle_set_error(output_error, "A requested video, audio, or subtitle track is unavailable.");
        return KMB_UNSUPPORTED;
    }
    if (kmb_subtitle_is_hdr(pipeline->input->streams[pipeline->selected_video_track_id]->codecpar)) {
        kmb_subtitle_set_error(
            output_error,
            "Subtitle burn-in refuses HDR, HLG, and Dolby Vision until a controlled 10-bit color pipeline is selected."
        );
        return KMB_UNSUPPORTED;
    }
    if (start_time_us > 0) {
        result = avformat_seek_file(
            pipeline->input,
            -1,
            INT64_MIN,
            start_time_us,
            start_time_us,
            AVSEEK_FLAG_BACKWARD
        );
        if (result < 0) {
            kmb_subtitle_set_av_error(output_error, "Could not seek media input", result);
            return KMB_READ_FAILED;
        }
        avformat_flush(pipeline->input);
    }
    return KMB_OK;
}

static KmbResult kmb_subtitle_open_decoder(KmbSubtitlePipeline *pipeline, char **output_error) {
    AVStream *stream = pipeline->input->streams[pipeline->selected_video_track_id];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    int result = 0;
    if (codec == NULL) {
        kmb_subtitle_set_error(output_error, "The selected runtime has no decoder for the video track.");
        return KMB_UNSUPPORTED;
    }
    pipeline->decoder = avcodec_alloc_context3(codec);
    if (pipeline->decoder == NULL) {
        kmb_subtitle_set_error(output_error, "Could not allocate the video decoder.");
        return KMB_ALLOCATION_FAILED;
    }
    result = avcodec_parameters_to_context(pipeline->decoder, stream->codecpar);
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not configure the video decoder", result);
        return KMB_STREAM_INFO_FAILED;
    }
    pipeline->decoder->pkt_timebase = stream->time_base;
    result = avcodec_open2(pipeline->decoder, codec, NULL);
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not open the video decoder", result);
        return KMB_UNSUPPORTED;
    }
    return KMB_OK;
}

static KmbResult kmb_subtitle_open_encoder(KmbSubtitlePipeline *pipeline, char **output_error) {
    KmbAvcEncoderAttempt attempts[8] = {0};
    AVStream *input_stream = pipeline->input->streams[pipeline->selected_video_track_id];
    AVRational frame_rate = av_guess_frame_rate(pipeline->input, input_stream, NULL);
    double frames_per_second = 30.0;
    int64_t bitrate = 0;
    int attempt_count = 0;
    int attempt_index = 0;
    int found_encoder = 0;
    int last_result = AVERROR_ENCODER_NOT_FOUND;
    if (frame_rate.num <= 0 || frame_rate.den <= 0) frame_rate = (AVRational){30, 1};
    frames_per_second = av_q2d(frame_rate);
    if (!isfinite(frames_per_second) || frames_per_second <= 0.0 || frames_per_second > 240.0) {
        frame_rate = (AVRational){30, 1};
        frames_per_second = 30.0;
    }
    bitrate = (int64_t)(pipeline->decoder->width * (int64_t)pipeline->decoder->height *
        frames_per_second / 8.0);
    if (bitrate < 2000000) bitrate = 2000000;
    if (bitrate > 16000000) bitrate = 16000000;
    attempt_count = kmb_avc_encoder_attempts(attempts, 8);
    for (attempt_index = 0; attempt_index < attempt_count; ++attempt_index) {
        const KmbAvcEncoderAttempt *attempt = &attempts[attempt_index];
        const AVCodec *codec = kmb_avc_encoder_find(attempt);
        AVCodecContext *context = NULL;
        AVDictionary *options = NULL;
        if (codec == NULL) continue;
        found_encoder = 1;
        context = avcodec_alloc_context3(codec);
        if (context == NULL) {
            kmb_subtitle_set_error(output_error, "Could not allocate the AVC encoder.");
            return KMB_ALLOCATION_FAILED;
        }
        context->width = pipeline->decoder->width;
        context->height = pipeline->decoder->height;
        context->sample_aspect_ratio = pipeline->decoder->sample_aspect_ratio;
        if (context->sample_aspect_ratio.num <= 0 || context->sample_aspect_ratio.den <= 0) {
            context->sample_aspect_ratio = (AVRational){1, 1};
        }
        context->pix_fmt = attempt->pixel_format;
        context->time_base = input_stream->time_base;
        if (context->time_base.num <= 0 || context->time_base.den <= 0) {
            context->time_base = av_inv_q(frame_rate);
        }
        context->framerate = frame_rate;
        context->bit_rate = bitrate;
        context->gop_size = (int)(frames_per_second * 2.0 + 0.5);
        context->max_b_frames = 0;
        context->color_range = AVCOL_RANGE_MPEG;
        context->color_primaries = AVCOL_PRI_BT709;
        context->color_trc = AVCOL_TRC_BT709;
        context->colorspace = AVCOL_SPC_BT709;
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        kmb_avc_encoder_apply_options(attempt, &options);
        last_result = avcodec_open2(context, codec, &options);
        av_dict_free(&options);
        if (last_result >= 0) {
            pipeline->encoder = context;
            return KMB_OK;
        }
        avcodec_free_context(&context);
    }
    if (!found_encoder) {
        kmb_subtitle_set_error(output_error, "No supported LGPL-compatible AVC encoder is available.");
    } else {
        kmb_subtitle_set_av_error(output_error, "Could not open an AVC encoder", last_result);
    }
    return KMB_UNSUPPORTED;
}

static KmbResult kmb_subtitle_create_filter_graph(
    KmbSubtitlePipeline *pipeline,
    const char *input_locator,
    char **output_error
) {
    const AVFilter *buffer = avfilter_get_by_name("buffer");
    const AVFilter *subtitles = avfilter_get_by_name("subtitles");
    const AVFilter *scale = avfilter_get_by_name("scale");
    const AVFilter *format = avfilter_get_by_name("format");
    const AVFilter *buffer_sink = avfilter_get_by_name("buffersink");
    AVFilterContext *subtitle_context = NULL;
    AVFilterContext *scale_context = NULL;
    AVFilterContext *format_context = NULL;
    AVStream *stream = pipeline->input->streams[pipeline->selected_video_track_id];
    AVRational aspect = pipeline->decoder->sample_aspect_ratio;
    char source_arguments[512] = {0};
    char scale_arguments[256] = {0};
    const AVCodecParameters *parameters = stream->codecpar;
    enum AVColorSpace input_colorspace = kmb_subtitle_sdr_colorspace(parameters);
    enum AVColorRange input_range = kmb_subtitle_sdr_range(parameters);
    int subtitle_ordinal = kmb_subtitle_ordinal(pipeline->input, pipeline->selected_subtitle_track_id);
    int result = 0;
    if (buffer == NULL || subtitles == NULL || scale == NULL || format == NULL || buffer_sink == NULL ||
        subtitle_ordinal < 0) {
        kmb_subtitle_set_error(output_error, "The runtime lacks a required libass video filter component.");
        return KMB_UNSUPPORTED;
    }
    if (aspect.num <= 0 || aspect.den <= 0) {
        aspect = (AVRational){1, 1};
    }
    pipeline->filter_graph = avfilter_graph_alloc();
    if (pipeline->filter_graph == NULL) {
        kmb_subtitle_set_error(output_error, "Could not allocate the subtitle filter graph.");
        return KMB_ALLOCATION_FAILED;
    }
    snprintf(
        source_arguments,
        sizeof(source_arguments),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:colorspace=%d:range=%d",
        pipeline->decoder->width,
        pipeline->decoder->height,
        pipeline->decoder->pix_fmt,
        stream->time_base.num,
        stream->time_base.den,
        aspect.num,
        aspect.den,
        input_colorspace,
        input_range
    );
    result = avfilter_graph_create_filter(
        &pipeline->buffer_source,
        buffer,
        "video_input",
        source_arguments,
        NULL,
        pipeline->filter_graph
    );
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not create the video buffer filter", result);
        return KMB_UNSUPPORTED;
    }
    subtitle_context = avfilter_graph_alloc_filter(pipeline->filter_graph, subtitles, "subtitle_compositor");
    if (subtitle_context == NULL) {
        kmb_subtitle_set_error(output_error, "Could not allocate the libass subtitle filter.");
        return KMB_ALLOCATION_FAILED;
    }
    result = av_opt_set(subtitle_context, "filename", input_locator, AV_OPT_SEARCH_CHILDREN);
    if (result >= 0) {
        result = av_opt_set_int(subtitle_context, "stream_index", subtitle_ordinal, AV_OPT_SEARCH_CHILDREN);
    }
    if (result >= 0) {
        result = avfilter_init_str(subtitle_context, NULL);
    }
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not initialize the selected subtitle track", result);
        return KMB_UNSUPPORTED;
    }
    snprintf(
        scale_arguments,
        sizeof(scale_arguments),
        "in_color_matrix=%d:out_color_matrix=%d:in_range=%d:out_range=%d",
        input_colorspace,
        AVCOL_SPC_BT709,
        input_range,
        AVCOL_RANGE_MPEG
    );
    result = avfilter_graph_create_filter(
        &scale_context,
        scale,
        "sdr_scale",
        scale_arguments,
        NULL,
        pipeline->filter_graph
    );
    if (result >= 0) {
        result = avfilter_graph_create_filter(
            &format_context,
            format,
            "encoder_format",
            "pix_fmts=yuv420p",
            NULL,
            pipeline->filter_graph
        );
    }
    if (result >= 0) {
        result = avfilter_graph_create_filter(
            &pipeline->buffer_sink,
            buffer_sink,
            "video_output",
            NULL,
            NULL,
            pipeline->filter_graph
        );
    }
    if (result >= 0) {
        result = avfilter_link(pipeline->buffer_source, 0, subtitle_context, 0);
    }
    if (result >= 0) {
        result = avfilter_link(subtitle_context, 0, scale_context, 0);
    }
    if (result >= 0) {
        result = avfilter_link(scale_context, 0, format_context, 0);
    }
    if (result >= 0) {
        result = avfilter_link(format_context, 0, pipeline->buffer_sink, 0);
    }
    if (result >= 0) {
        result = avfilter_graph_config(pipeline->filter_graph, NULL);
    }
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not configure the subtitle filter graph", result);
        return KMB_UNSUPPORTED;
    }
    return KMB_OK;
}

static KmbResult kmb_subtitle_open_output(
    KmbSubtitlePipeline *pipeline,
    int64_t fragment_duration_us,
    char **output_error
) {
    AVStream *video_output = NULL;
    int result = avformat_alloc_output_context2(&pipeline->output, NULL, "mp4", NULL);
    if (result < 0 || pipeline->output == NULL) {
        kmb_subtitle_set_av_error(output_error, "Could not create fragmented MP4 output", result);
        return KMB_OPEN_OUTPUT_FAILED;
    }
    video_output = avformat_new_stream(pipeline->output, NULL);
    if (video_output == NULL) {
        kmb_subtitle_set_error(output_error, "Could not create the encoded video output stream.");
        return KMB_ALLOCATION_FAILED;
    }
    pipeline->output_video_track_id = video_output->index;
    video_output->time_base = pipeline->encoder->time_base;
    result = avcodec_parameters_from_context(video_output->codecpar, pipeline->encoder);
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not publish AVC encoder parameters", result);
        return KMB_OPEN_OUTPUT_FAILED;
    }
    video_output->codecpar->codec_tag = MKTAG('a', 'v', 'c', '1');
    pipeline->output_audio_track_id = -1;
    if (pipeline->selected_audio_track_id >= 0) {
        AVStream *audio_input = pipeline->input->streams[pipeline->selected_audio_track_id];
        AVStream *audio_output = NULL;
        if (avformat_query_codec(
                pipeline->output->oformat,
                audio_input->codecpar->codec_id,
                FF_COMPLIANCE_NORMAL
            ) <= 0) {
            kmb_subtitle_set_error(output_error, "The selected audio track cannot be copied into fragmented MP4.");
            return KMB_UNSUPPORTED;
        }
        audio_output = avformat_new_stream(pipeline->output, NULL);
        if (audio_output == NULL) {
            kmb_subtitle_set_error(output_error, "Could not create the copied audio output stream.");
            return KMB_ALLOCATION_FAILED;
        }
        pipeline->output_audio_track_id = audio_output->index;
        audio_output->time_base = audio_input->time_base;
        result = avcodec_parameters_copy(audio_output->codecpar, audio_input->codecpar);
        if (result < 0) {
            kmb_subtitle_set_av_error(output_error, "Could not copy audio stream parameters", result);
            return KMB_OPEN_OUTPUT_FAILED;
        }
        audio_output->codecpar->codec_tag = 0;
    }
    pipeline->custom_buffer = av_malloc(32 * 1024);
    if (pipeline->custom_buffer == NULL) {
        kmb_subtitle_set_error(output_error, "Could not allocate the output callback buffer.");
        return KMB_ALLOCATION_FAILED;
    }
    pipeline->custom_io = avio_alloc_context(
        pipeline->custom_buffer,
        32 * 1024,
        1,
        &pipeline->write_state,
        NULL,
        kmb_subtitle_write_packet,
        NULL
    );
    if (pipeline->custom_io == NULL) {
        kmb_subtitle_set_error(output_error, "Could not create the output callback context.");
        return KMB_ALLOCATION_FAILED;
    }
    pipeline->custom_buffer = NULL;
    pipeline->output->pb = pipeline->custom_io;
    pipeline->output->flags |= AVFMT_FLAG_CUSTOM_IO;
    av_dict_set(
        &pipeline->muxer_options,
        "movflags",
        "frag_keyframe+delay_moov+default_base_moof+negative_cts_offsets",
        0
    );
    av_dict_set_int(&pipeline->muxer_options, "frag_duration", fragment_duration_us, 0);
    result = avformat_write_header(pipeline->output, &pipeline->muxer_options);
    if (result < 0) {
        if (pipeline->write_state.cancelled) {
            return KMB_CANCELLED;
        }
        kmb_subtitle_set_av_error(output_error, "Could not write the fragmented MP4 header", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_subtitle_write_encoded_packets(KmbSubtitlePipeline *pipeline, char **output_error) {
    int result = 0;
    while ((result = avcodec_receive_packet(pipeline->encoder, pipeline->encoded_packet)) >= 0) {
        AVStream *output_stream = pipeline->output->streams[pipeline->output_video_track_id];
        if (pipeline->encoded_packet->duration <= 0 &&
            pipeline->encoder->framerate.num > 0 && pipeline->encoder->framerate.den > 0) {
            pipeline->encoded_packet->duration = av_rescale_q(
                1,
                av_inv_q(pipeline->encoder->framerate),
                pipeline->encoder->time_base
            );
        }
        av_packet_rescale_ts(
            pipeline->encoded_packet,
            pipeline->encoder->time_base,
            output_stream->time_base
        );
        pipeline->encoded_packet->stream_index = pipeline->output_video_track_id;
        pipeline->encoded_packet->pos = -1;
        result = av_interleaved_write_frame(pipeline->output, pipeline->encoded_packet);
        av_packet_unref(pipeline->encoded_packet);
        if (result < 0) {
            if (pipeline->write_state.cancelled) {
                return KMB_CANCELLED;
            }
            kmb_subtitle_set_av_error(output_error, "Could not write an encoded video packet", result);
            return KMB_WRITE_FAILED;
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_subtitle_set_av_error(output_error, "Could not receive an encoded video packet", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_subtitle_encode_filtered_frames(KmbSubtitlePipeline *pipeline, char **output_error) {
    int result = 0;
    while ((result = av_buffersink_get_frame(pipeline->buffer_sink, pipeline->filtered_frame)) >= 0) {
        pipeline->filtered_frame->pict_type = AV_PICTURE_TYPE_NONE;
        pipeline->filtered_frame->color_range = AVCOL_RANGE_MPEG;
        pipeline->filtered_frame->color_primaries = AVCOL_PRI_BT709;
        pipeline->filtered_frame->color_trc = AVCOL_TRC_BT709;
        pipeline->filtered_frame->colorspace = AVCOL_SPC_BT709;
        result = avcodec_send_frame(pipeline->encoder, pipeline->filtered_frame);
        int retry_count = 0;
        while (result == AVERROR(EAGAIN) && retry_count++ < KMB_CODEC_EAGAIN_RETRY_LIMIT) {
            KmbResult bridge_result = kmb_subtitle_write_encoded_packets(pipeline, output_error);
            if (bridge_result != KMB_OK) {
                av_frame_unref(pipeline->filtered_frame);
                return bridge_result;
            }
            if (pipeline->write_state.cancelled) {
                av_frame_unref(pipeline->filtered_frame);
                return KMB_CANCELLED;
            }
            av_usleep(KMB_CODEC_EAGAIN_RETRY_DELAY_US);
            result = avcodec_send_frame(pipeline->encoder, pipeline->filtered_frame);
        }
        av_frame_unref(pipeline->filtered_frame);
        if (result < 0) {
            kmb_subtitle_set_av_error(output_error, "Could not submit a composited video frame", result);
            return KMB_WRITE_FAILED;
        }
        {
            KmbResult bridge_result = kmb_subtitle_write_encoded_packets(pipeline, output_error);
            if (bridge_result != KMB_OK) {
                return bridge_result;
            }
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_subtitle_set_av_error(output_error, "Could not read a composited video frame", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_subtitle_receive_decoded_frames(
    KmbSubtitlePipeline *pipeline,
    char **output_error
) {
    int result = 0;
    while ((result = avcodec_receive_frame(pipeline->decoder, pipeline->decoded_frame)) >= 0) {
        pipeline->decoded_frame->pts = pipeline->decoded_frame->best_effort_timestamp;
        result = av_buffersrc_add_frame_flags(
            pipeline->buffer_source,
            pipeline->decoded_frame,
            AV_BUFFERSRC_FLAG_KEEP_REF
        );
        av_frame_unref(pipeline->decoded_frame);
        if (result < 0) {
            kmb_subtitle_set_av_error(output_error, "Could not submit a decoded video frame", result);
            return KMB_WRITE_FAILED;
        }
        {
            KmbResult bridge_result = kmb_subtitle_encode_filtered_frames(pipeline, output_error);
            if (bridge_result != KMB_OK) return bridge_result;
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_subtitle_set_av_error(output_error, "Could not decode a video frame", result);
        return KMB_READ_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_subtitle_decode_video_packet(
    KmbSubtitlePipeline *pipeline,
    const AVPacket *packet,
    char **output_error
) {
    int result = avcodec_send_packet(pipeline->decoder, packet);
    int retry_count = 0;
    while (result == AVERROR(EAGAIN) && retry_count++ < KMB_CODEC_EAGAIN_RETRY_LIMIT) {
        KmbResult bridge_result = kmb_subtitle_receive_decoded_frames(pipeline, output_error);
        if (bridge_result != KMB_OK) return bridge_result;
        if (pipeline->write_state.cancelled) return KMB_CANCELLED;
        av_usleep(KMB_CODEC_EAGAIN_RETRY_DELAY_US);
        result = avcodec_send_packet(pipeline->decoder, packet);
    }
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not submit a compressed video packet", result);
        return KMB_READ_FAILED;
    }
    return kmb_subtitle_receive_decoded_frames(pipeline, output_error);
}

static KmbResult kmb_subtitle_copy_audio_packet(
    KmbSubtitlePipeline *pipeline,
    AVPacket *packet,
    char **output_error
) {
    AVStream *input_stream = pipeline->input->streams[pipeline->selected_audio_track_id];
    AVStream *output_stream = pipeline->output->streams[pipeline->output_audio_track_id];
    int result = 0;
    kmb_prepare_packet_timestamps(input_stream, packet, &pipeline->audio_timestamp_state);
    av_packet_rescale_ts(packet, input_stream->time_base, output_stream->time_base);
    packet->stream_index = pipeline->output_audio_track_id;
    packet->pos = -1;
    result = av_interleaved_write_frame(pipeline->output, packet);
    if (result < 0) {
        if (pipeline->write_state.cancelled) {
            return KMB_CANCELLED;
        }
        kmb_subtitle_set_av_error(output_error, "Could not write a copied audio packet", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_subtitle_flush(KmbSubtitlePipeline *pipeline, char **output_error) {
    KmbResult bridge_result = kmb_subtitle_decode_video_packet(pipeline, NULL, output_error);
    int result = 0;
    if (bridge_result != KMB_OK) {
        return bridge_result;
    }
    result = av_buffersrc_add_frame_flags(pipeline->buffer_source, NULL, 0);
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not flush the subtitle filter", result);
        return KMB_WRITE_FAILED;
    }
    bridge_result = kmb_subtitle_encode_filtered_frames(pipeline, output_error);
    if (bridge_result != KMB_OK) {
        return bridge_result;
    }
    result = avcodec_send_frame(pipeline->encoder, NULL);
    if (result < 0) {
        kmb_subtitle_set_av_error(output_error, "Could not flush the AVC encoder", result);
        return KMB_WRITE_FAILED;
    }
    return kmb_subtitle_write_encoded_packets(pipeline, output_error);
}

static KmbResult kmb_subtitle_run(KmbSubtitlePipeline *pipeline, char **output_error) {
    int result = 0;
    KmbResult bridge_result = KMB_OK;
    while ((result = av_read_frame(pipeline->input, pipeline->input_packet)) >= 0) {
        if (pipeline->input_packet->stream_index == pipeline->selected_video_track_id) {
            bridge_result = kmb_subtitle_decode_video_packet(pipeline, pipeline->input_packet, output_error);
        } else if (pipeline->input_packet->stream_index == pipeline->selected_audio_track_id) {
            bridge_result = kmb_subtitle_copy_audio_packet(pipeline, pipeline->input_packet, output_error);
        }
        av_packet_unref(pipeline->input_packet);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
    }
    if (result != AVERROR_EOF) {
        kmb_subtitle_set_av_error(output_error, "Could not read a media packet", result);
        return KMB_READ_FAILED;
    }
    bridge_result = kmb_subtitle_flush(pipeline, output_error);
    if (bridge_result != KMB_OK) {
        return bridge_result;
    }
    result = av_write_trailer(pipeline->output);
    if (result < 0) {
        if (pipeline->write_state.cancelled) {
            return KMB_CANCELLED;
        }
        kmb_subtitle_set_av_error(output_error, "Could not finalize fragmented MP4 output", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

KmbResult kmb_burn_subtitles_fragmented_mp4_stream(
    const char *input_locator,
    int64_t fragment_duration_us,
    int64_t start_time_us,
    int32_t preferred_video_track_id,
    int32_t preferred_audio_track_id,
    int32_t preferred_subtitle_track_id,
    KmbWriteCallback write_callback,
    void *opaque,
    char **output_error
) {
    KmbSubtitlePipeline pipeline = {0};
    KmbResult bridge_result = KMB_OK;
    if (output_error != NULL) {
        *output_error = NULL;
    }
    if (input_locator == NULL || fragment_duration_us <= 0 || start_time_us < 0 ||
        preferred_subtitle_track_id < 0 || write_callback == NULL) {
        kmb_subtitle_set_error(output_error, "Valid input, subtitle track, callback, duration, and start time are required.");
        return KMB_INVALID_ARGUMENT;
    }
    pipeline.write_state = (KmbSubtitleWriteState){write_callback, opaque, 0};
    bridge_result = kmb_subtitle_open_input(
        &pipeline,
        input_locator,
        preferred_video_track_id,
        preferred_audio_track_id,
        preferred_subtitle_track_id,
        start_time_us,
        output_error
    );
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_subtitle_open_decoder(&pipeline, output_error);
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_subtitle_open_encoder(&pipeline, output_error);
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_subtitle_create_filter_graph(&pipeline, input_locator, output_error);
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_subtitle_open_output(&pipeline, fragment_duration_us, output_error);
    }
    if (bridge_result == KMB_OK) {
        pipeline.input_packet = av_packet_alloc();
        pipeline.encoded_packet = av_packet_alloc();
        pipeline.decoded_frame = av_frame_alloc();
        pipeline.filtered_frame = av_frame_alloc();
        if (pipeline.input_packet == NULL || pipeline.encoded_packet == NULL ||
            pipeline.decoded_frame == NULL || pipeline.filtered_frame == NULL) {
            kmb_subtitle_set_error(output_error, "Could not allocate subtitle transcoding frames and packets.");
            bridge_result = KMB_ALLOCATION_FAILED;
        }
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_subtitle_run(&pipeline, output_error);
    }
    kmb_subtitle_cleanup(&pipeline);
    return bridge_result;
}

#else

KmbResult kmb_burn_subtitles_fragmented_mp4_stream(
    const char *input_locator,
    int64_t fragment_duration_us,
    int64_t start_time_us,
    int32_t preferred_video_track_id,
    int32_t preferred_audio_track_id,
    int32_t preferred_subtitle_track_id,
    KmbWriteCallback write_callback,
    void *opaque,
    char **output_error
) {
    (void)input_locator;
    (void)fragment_duration_us;
    (void)start_time_us;
    (void)preferred_video_track_id;
    (void)preferred_audio_track_id;
    (void)preferred_subtitle_track_id;
    (void)write_callback;
    (void)opaque;
    if (output_error != NULL) {
        *output_error = av_strdup("This runtime does not include the optional libass subtitle burn-in pipeline.");
    }
    return KMB_UNSUPPORTED;
}

#endif
