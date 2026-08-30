/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "kmedia_bridge.h"
#include "kmedia_bridge_timestamps.h"

#include <libavutil/mem.h>

#if defined(KMB_ENABLE_AVFOUNDATION_TRANSCODE)

#include "kmedia_bridge_avc_encoder.h"

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

typedef struct KmbAvfWriteState {
    KmbWriteCallback callback;
    void *opaque;
    int cancelled;
} KmbAvfWriteState;

typedef struct KmbAvfPipeline {
    AVFormatContext *input;
    AVFormatContext *output;
    AVCodecContext *video_decoder;
    AVCodecContext *video_encoder;
    AVCodecContext *audio_decoder;
    AVCodecContext *audio_encoder;
    AVFilterGraph *video_filter_graph;
    AVFilterContext *video_buffer_source;
    AVFilterContext *video_buffer_sink;
    SwrContext *audio_resampler;
    AVAudioFifo *audio_fifo;
    AVPacket *input_packet;
    AVPacket *video_packet;
    AVPacket *audio_packet;
    AVFrame *video_decoded_frame;
    AVFrame *video_filtered_frame;
    AVFrame *audio_decoded_frame;
    AVIOContext *custom_io;
    unsigned char *custom_buffer;
    AVDictionary *muxer_options;
    int selected_video_track_id;
    int selected_audio_track_id;
    int selected_subtitle_track_id;
    int output_video_track_id;
    int output_audio_track_id;
    int64_t next_video_pts;
    int video_pts_initialized;
    int64_t next_audio_pts;
    int audio_pts_initialized;
    int synthesize_silent_audio;
    int64_t requested_start_time_us;
    KmbAvfWriteState write_state;
    KmbProgressCallback progress_callback;
    void *progress_opaque;
} KmbAvfPipeline;

static void kmb_avf_set_error(char **output_error, const char *message) {
    if (output_error != NULL) {
        *output_error = av_strdup(message != NULL ? message : "Unknown AVC/AAC compatibility conversion error.");
    }
}

static void kmb_avf_set_av_error(char **output_error, const char *operation, int error_code) {
    char error_text[AV_ERROR_MAX_STRING_SIZE] = {0};
    char combined[320] = {0};
    av_strerror(error_code, error_text, sizeof(error_text));
    av_strlcpy(combined, operation, sizeof(combined));
    av_strlcat(combined, ": ", sizeof(combined));
    av_strlcat(combined, error_text, sizeof(combined));
    kmb_avf_set_error(output_error, combined);
}

static int kmb_avf_select_track(
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

static int kmb_avf_subtitle_ordinal(const AVFormatContext *input, int absolute_track_id) {
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

static int kmb_avf_is_hdr(const AVCodecParameters *parameters) {
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

static enum AVColorSpace kmb_avf_sdr_colorspace(const AVCodecParameters *parameters) {
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

static enum AVColorRange kmb_avf_sdr_range(const AVCodecParameters *parameters) {
    return parameters->color_range == AVCOL_RANGE_JPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

static int kmb_avf_write_packet(void *opaque, const uint8_t *bytes, int size) {
    KmbAvfWriteState *state = (KmbAvfWriteState *)opaque;
    if (state == NULL || state->callback == NULL || size < 0) {
        return AVERROR(EINVAL);
    }
    if (state->callback(state->opaque, bytes, size) != 0) {
        state->cancelled = 1;
        return AVERROR_EXIT;
    }
    return size;
}

static void kmb_avf_cleanup(KmbAvfPipeline *pipeline) {
    av_dict_free(&pipeline->muxer_options);
    av_packet_free(&pipeline->input_packet);
    av_packet_free(&pipeline->video_packet);
    av_packet_free(&pipeline->audio_packet);
    av_frame_free(&pipeline->video_decoded_frame);
    av_frame_free(&pipeline->video_filtered_frame);
    av_frame_free(&pipeline->audio_decoded_frame);
    av_audio_fifo_free(pipeline->audio_fifo);
    swr_free(&pipeline->audio_resampler);
    avfilter_graph_free(&pipeline->video_filter_graph);
    avcodec_free_context(&pipeline->video_decoder);
    avcodec_free_context(&pipeline->video_encoder);
    avcodec_free_context(&pipeline->audio_decoder);
    avcodec_free_context(&pipeline->audio_encoder);
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

static KmbResult kmb_avf_open_input(
    KmbAvfPipeline *pipeline,
    const char *input_locator,
    int preferred_video_track_id,
    int preferred_audio_track_id,
    int preferred_subtitle_track_id,
    int64_t start_time_us,
    char **output_error
) {
    int result = avformat_open_input(&pipeline->input, input_locator, NULL, NULL);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not open media input", result);
        return KMB_OPEN_INPUT_FAILED;
    }
    result = avformat_find_stream_info(pipeline->input, NULL);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not read media stream information", result);
        return KMB_STREAM_INFO_FAILED;
    }
    pipeline->selected_video_track_id =
        kmb_avf_select_track(pipeline->input, AVMEDIA_TYPE_VIDEO, preferred_video_track_id);
    pipeline->selected_audio_track_id = -1;
    if (preferred_audio_track_id != -2) {
        pipeline->selected_audio_track_id =
            kmb_avf_select_track(pipeline->input, AVMEDIA_TYPE_AUDIO, preferred_audio_track_id);
    }
    pipeline->selected_subtitle_track_id = -1;
    if (preferred_subtitle_track_id != -2) {
        pipeline->selected_subtitle_track_id =
            kmb_avf_select_track(pipeline->input, AVMEDIA_TYPE_SUBTITLE, preferred_subtitle_track_id);
    }
    if (pipeline->selected_video_track_id < 0 || pipeline->selected_audio_track_id == -2 ||
        pipeline->selected_subtitle_track_id == -2) {
        kmb_avf_set_error(output_error, "A requested video, audio, or subtitle track is unavailable.");
        return KMB_UNSUPPORTED;
    }
    if (kmb_avf_is_hdr(pipeline->input->streams[pipeline->selected_video_track_id]->codecpar)) {
        kmb_avf_set_error(
            output_error,
            "The AVC/AAC compatibility path accepts SDR input; explicit HDR must use the controlled tone mapper."
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
            kmb_avf_set_av_error(output_error, "Could not seek media input", result);
            return KMB_READ_FAILED;
        }
        avformat_flush(pipeline->input);
    }
    return KMB_OK;
}

static KmbResult kmb_avf_open_decoder(
    KmbAvfPipeline *pipeline,
    int stream_id,
    AVCodecContext **decoder,
    const char *media_name,
    char **output_error
) {
    AVStream *stream = pipeline->input->streams[stream_id];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    int result = 0;
    if (codec == NULL) {
        char message[192] = {0};
        snprintf(message, sizeof(message), "The selected runtime has no decoder for the %s track.", media_name);
        kmb_avf_set_error(output_error, message);
        return KMB_UNSUPPORTED;
    }
    *decoder = avcodec_alloc_context3(codec);
    if (*decoder == NULL) {
        kmb_avf_set_error(output_error, "Could not allocate a decoder.");
        return KMB_ALLOCATION_FAILED;
    }
    result = avcodec_parameters_to_context(*decoder, stream->codecpar);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not configure a decoder", result);
        return KMB_STREAM_INFO_FAILED;
    }
    (*decoder)->pkt_timebase = stream->time_base;
    result = avcodec_open2(*decoder, codec, NULL);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not open a decoder", result);
        return KMB_UNSUPPORTED;
    }
    return KMB_OK;
}

static void kmb_avf_compatibility_dimensions(
    int input_width,
    int input_height,
    int *output_width,
    int *output_height
) {
    const int maximum_width = 1920;
    const int maximum_height = 1080;
    double scale = 1.0;
    int scaled_width = input_width;
    int scaled_height = input_height;
    if (input_width > maximum_width || input_height > maximum_height) {
        scale = fmin(
            maximum_width / (double)input_width,
            maximum_height / (double)input_height
        );
        scaled_width = (int)floor(input_width * scale);
        scaled_height = (int)floor(input_height * scale);
        scaled_width -= scaled_width & 1;
        scaled_height -= scaled_height & 1;
    }
    *output_width = scaled_width < 2 ? 2 : scaled_width;
    *output_height = scaled_height < 2 ? 2 : scaled_height;
}

static KmbResult kmb_avf_open_video_encoder(KmbAvfPipeline *pipeline, char **output_error) {
    KmbAvcEncoderAttempt attempts[8] = {0};
    AVStream *input_stream = pipeline->input->streams[pipeline->selected_video_track_id];
    AVRational frame_rate = av_guess_frame_rate(pipeline->input, input_stream, NULL);
    double frames_per_second = av_q2d(frame_rate);
    int64_t bitrate = 0;
    int output_width = 0;
    int output_height = 0;
    int attempt_count = 0;
    int attempt_index = 0;
    int found_encoder = 0;
    int last_result = AVERROR_ENCODER_NOT_FOUND;
    if (pipeline->video_decoder->width <= 0 || pipeline->video_decoder->height <= 0 ||
        (pipeline->video_decoder->width & 1) != 0 || (pipeline->video_decoder->height & 1) != 0) {
        kmb_avf_set_error(output_error, "The video dimensions cannot be represented by AVC 4:2:0 output.");
        return KMB_UNSUPPORTED;
    }
    if (!isfinite(frames_per_second) || frames_per_second <= 0.0) {
        frame_rate = (AVRational){30, 1};
        frames_per_second = 30.0;
    }
    kmb_avf_compatibility_dimensions(
        pipeline->video_decoder->width,
        pipeline->video_decoder->height,
        &output_width,
        &output_height
    );
    bitrate = (int64_t)(output_width * (int64_t)output_height * frames_per_second / 8.0);
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
            kmb_avf_set_error(output_error, "Could not allocate the AVC encoder.");
            return KMB_ALLOCATION_FAILED;
        }
        context->width = output_width;
        context->height = output_height;
        context->sample_aspect_ratio = pipeline->video_decoder->sample_aspect_ratio;
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
            pipeline->video_encoder = context;
            return KMB_OK;
        }
        avcodec_free_context(&context);
    }
    if (!found_encoder) {
        kmb_avf_set_error(output_error, "No supported LGPL-compatible AVC encoder is available.");
    } else {
        kmb_avf_set_av_error(output_error, "Could not open an AVC encoder", last_result);
    }
    return KMB_UNSUPPORTED;
}

static KmbResult kmb_avf_create_video_filter(
    KmbAvfPipeline *pipeline,
    const char *input_locator,
    char **output_error
) {
    const AVFilter *buffer = avfilter_get_by_name("buffer");
    const AVFilter *scale = avfilter_get_by_name("scale");
    const AVFilter *format = avfilter_get_by_name("format");
    const AVFilter *buffer_sink = avfilter_get_by_name("buffersink");
    AVFilterContext *previous = NULL;
    AVFilterContext *subtitle_context = NULL;
    AVFilterContext *scale_context = NULL;
    AVFilterContext *format_context = NULL;
    AVStream *stream = pipeline->input->streams[pipeline->selected_video_track_id];
    const AVCodecParameters *parameters = stream->codecpar;
    AVRational aspect = pipeline->video_decoder->sample_aspect_ratio;
    enum AVPixelFormat pixel_format = pipeline->video_decoder->pix_fmt;
    enum AVColorSpace input_colorspace = kmb_avf_sdr_colorspace(parameters);
    enum AVColorRange input_range = kmb_avf_sdr_range(parameters);
    char source_arguments[512] = {0};
    char scale_arguments[256] = {0};
    int result = 0;
    if (pixel_format == AV_PIX_FMT_NONE && parameters->format >= 0) {
        pixel_format = (enum AVPixelFormat)parameters->format;
    }
    if (buffer == NULL || scale == NULL || format == NULL || buffer_sink == NULL ||
        pixel_format == AV_PIX_FMT_NONE) {
        kmb_avf_set_error(output_error, "The runtime lacks a required SDR video conversion component.");
        return KMB_UNSUPPORTED;
    }
    if (aspect.num <= 0 || aspect.den <= 0) {
        aspect = (AVRational){1, 1};
    }
    pipeline->video_filter_graph = avfilter_graph_alloc();
    if (pipeline->video_filter_graph == NULL) {
        kmb_avf_set_error(output_error, "Could not allocate the SDR video filter graph.");
        return KMB_ALLOCATION_FAILED;
    }
    snprintf(
        source_arguments,
        sizeof(source_arguments),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:colorspace=%d:range=%d",
        pipeline->video_decoder->width,
        pipeline->video_decoder->height,
        pixel_format,
        stream->time_base.num,
        stream->time_base.den,
        aspect.num,
        aspect.den,
        input_colorspace,
        input_range
    );
    result = avfilter_graph_create_filter(
        &pipeline->video_buffer_source,
        buffer,
        "video_input",
        source_arguments,
        NULL,
        pipeline->video_filter_graph
    );
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not create the video buffer filter", result);
        return KMB_UNSUPPORTED;
    }
    previous = pipeline->video_buffer_source;
    if (pipeline->selected_subtitle_track_id >= 0) {
        const AVFilter *subtitles = avfilter_get_by_name("subtitles");
        int subtitle_ordinal =
            kmb_avf_subtitle_ordinal(pipeline->input, pipeline->selected_subtitle_track_id);
        if (subtitles == NULL || subtitle_ordinal < 0) {
            kmb_avf_set_error(output_error, "The runtime lacks the selected text subtitle compositor.");
            return KMB_UNSUPPORTED;
        }
        subtitle_context =
            avfilter_graph_alloc_filter(pipeline->video_filter_graph, subtitles, "subtitle_compositor");
        if (subtitle_context == NULL) {
            kmb_avf_set_error(output_error, "Could not allocate the subtitle compositor.");
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
            kmb_avf_set_av_error(output_error, "Could not initialize the selected subtitle track", result);
            return KMB_UNSUPPORTED;
        }
        result = avfilter_link(previous, 0, subtitle_context, 0);
        if (result < 0) {
            kmb_avf_set_av_error(output_error, "Could not connect the subtitle compositor", result);
            return KMB_UNSUPPORTED;
        }
        previous = subtitle_context;
    }
    snprintf(
        scale_arguments,
        sizeof(scale_arguments),
        "w=%d:h=%d:in_color_matrix=%d:out_color_matrix=%d:in_range=%d:out_range=%d",
        pipeline->video_encoder->width,
        pipeline->video_encoder->height,
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
        pipeline->video_filter_graph
    );
    if (result >= 0) {
        result = avfilter_graph_create_filter(
            &format_context,
            format,
            "encoder_format",
            "pix_fmts=yuv420p",
            NULL,
            pipeline->video_filter_graph
        );
    }
    if (result >= 0) {
        result = avfilter_graph_create_filter(
            &pipeline->video_buffer_sink,
            buffer_sink,
            "video_output",
            NULL,
            NULL,
            pipeline->video_filter_graph
        );
    }
    if (result >= 0) {
        result = avfilter_link(previous, 0, scale_context, 0);
    }
    if (result >= 0) {
        result = avfilter_link(scale_context, 0, format_context, 0);
    }
    if (result >= 0) {
        result = avfilter_link(format_context, 0, pipeline->video_buffer_sink, 0);
    }
    if (result >= 0) {
        result = avfilter_graph_config(pipeline->video_filter_graph, NULL);
    }
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not configure the SDR video filter graph", result);
        return KMB_UNSUPPORTED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_open_audio_encoder(KmbAvfPipeline *pipeline, char **output_error) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    const enum AVSampleFormat *sample_formats = NULL;
    int sample_format_count = 0;
    int channels = pipeline->audio_decoder != NULL ? pipeline->audio_decoder->ch_layout.nb_channels : 2;
    int input_sample_rate = pipeline->audio_decoder != NULL ? pipeline->audio_decoder->sample_rate : 48000;
    int result = 0;
    if (codec == NULL) {
        kmb_avf_set_error(output_error, "The selected runtime has no AAC encoder.");
        return KMB_UNSUPPORTED;
    }
    if (channels <= 0 && pipeline->selected_audio_track_id >= 0) {
        channels = pipeline->input->streams[pipeline->selected_audio_track_id]->codecpar->ch_layout.nb_channels;
    }
    if (channels <= 0 || input_sample_rate <= 0) {
        kmb_avf_set_error(output_error, "The selected audio track has no usable channel layout or sample rate.");
        return KMB_UNSUPPORTED;
    }
    pipeline->audio_encoder = avcodec_alloc_context3(codec);
    if (pipeline->audio_encoder == NULL) {
        kmb_avf_set_error(output_error, "Could not allocate the AAC encoder.");
        return KMB_ALLOCATION_FAILED;
    }
    av_channel_layout_default(&pipeline->audio_encoder->ch_layout, channels == 1 ? 1 : 2);
    pipeline->audio_encoder->sample_rate = 48000;
    result = avcodec_get_supported_config(
        pipeline->audio_encoder,
        codec,
        AV_CODEC_CONFIG_SAMPLE_FORMAT,
        0,
        (const void **)&sample_formats,
        &sample_format_count
    );
    if (result < 0 || sample_formats == NULL || sample_format_count <= 0) {
        kmb_avf_set_av_error(output_error, "Could not query AAC sample formats", result);
        return KMB_UNSUPPORTED;
    }
    pipeline->audio_encoder->sample_fmt = sample_formats[0];
    pipeline->audio_encoder->bit_rate = channels == 1 ? 128000 : 192000;
    pipeline->audio_encoder->time_base = (AVRational){1, pipeline->audio_encoder->sample_rate};
    pipeline->audio_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    result = avcodec_open2(pipeline->audio_encoder, codec, NULL);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not open the AAC encoder", result);
        return KMB_UNSUPPORTED;
    }
    if (pipeline->audio_decoder == NULL) {
        pipeline->audio_pts_initialized = 1;
        pipeline->next_audio_pts = 0;
        return KMB_OK;
    }
    if (pipeline->audio_decoder->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&pipeline->audio_decoder->ch_layout, channels);
    }
    result = swr_alloc_set_opts2(
        &pipeline->audio_resampler,
        &pipeline->audio_encoder->ch_layout,
        pipeline->audio_encoder->sample_fmt,
        pipeline->audio_encoder->sample_rate,
        &pipeline->audio_decoder->ch_layout,
        pipeline->audio_decoder->sample_fmt,
        input_sample_rate,
        0,
        NULL
    );
    if (result < 0 || pipeline->audio_resampler == NULL) {
        kmb_avf_set_av_error(output_error, "Could not allocate the audio resampler", result);
        return KMB_ALLOCATION_FAILED;
    }
    result = swr_init(pipeline->audio_resampler);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not initialize the audio resampler", result);
        return KMB_UNSUPPORTED;
    }
    pipeline->audio_fifo = av_audio_fifo_alloc(
        pipeline->audio_encoder->sample_fmt,
        pipeline->audio_encoder->ch_layout.nb_channels,
        1
    );
    if (pipeline->audio_fifo == NULL) {
        kmb_avf_set_error(output_error, "Could not allocate the AAC sample queue.");
        return KMB_ALLOCATION_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_open_output(
    KmbAvfPipeline *pipeline,
    int64_t fragment_duration_us,
    const char *hls_playlist_path,
    const char *hls_segment_path_pattern,
    int maximum_playlist_segments,
    char **output_error
) {
    AVStream *video_output = NULL;
    const int is_mpeg_ts_hls = hls_playlist_path != NULL;
    int result = avformat_alloc_output_context2(
        &pipeline->output,
        NULL,
        is_mpeg_ts_hls ? "hls" : "mp4",
        hls_playlist_path
    );
    if (result < 0 || pipeline->output == NULL) {
        kmb_avf_set_av_error(
            output_error,
            is_mpeg_ts_hls ? "Could not create MPEG-TS HLS output" : "Could not create fragmented MP4 output",
            result
        );
        return KMB_OPEN_OUTPUT_FAILED;
    }
    video_output = avformat_new_stream(pipeline->output, NULL);
    if (video_output == NULL) {
        kmb_avf_set_error(output_error, "Could not create the AVC output stream.");
        return KMB_ALLOCATION_FAILED;
    }
    pipeline->output_video_track_id = video_output->index;
    video_output->time_base = pipeline->video_encoder->time_base;
    result = avcodec_parameters_from_context(video_output->codecpar, pipeline->video_encoder);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not publish AVC encoder parameters", result);
        return KMB_OPEN_OUTPUT_FAILED;
    }
    video_output->codecpar->codec_tag = is_mpeg_ts_hls ? 0 : MKTAG('a', 'v', 'c', '1');
    pipeline->output_audio_track_id = -1;
    if (pipeline->audio_encoder != NULL) {
        AVStream *audio_output = avformat_new_stream(pipeline->output, NULL);
        if (audio_output == NULL) {
            kmb_avf_set_error(output_error, "Could not create the AAC output stream.");
            return KMB_ALLOCATION_FAILED;
        }
        pipeline->output_audio_track_id = audio_output->index;
        audio_output->time_base = pipeline->audio_encoder->time_base;
        result = avcodec_parameters_from_context(audio_output->codecpar, pipeline->audio_encoder);
        if (result < 0) {
            kmb_avf_set_av_error(output_error, "Could not publish AAC encoder parameters", result);
            return KMB_OPEN_OUTPUT_FAILED;
        }
        audio_output->codecpar->codec_tag = 0;
    }
    if (is_mpeg_ts_hls) {
        char segment_duration_seconds[64] = {0};
        snprintf(
            segment_duration_seconds,
            sizeof(segment_duration_seconds),
            "%.6f",
            fragment_duration_us / 1000000.0
        );
        av_dict_set(&pipeline->muxer_options, "hls_time", segment_duration_seconds, 0);
        av_dict_set_int(&pipeline->muxer_options, "hls_list_size", maximum_playlist_segments, 0);
        av_dict_set_int(&pipeline->muxer_options, "hls_delete_threshold", 2, 0);
        av_dict_set(&pipeline->muxer_options, "hls_segment_type", "mpegts", 0);
        av_dict_set(
            &pipeline->muxer_options,
            "hls_flags",
            "delete_segments+independent_segments+temp_file",
            0
        );
        av_dict_set(&pipeline->muxer_options, "hls_segment_filename", hls_segment_path_pattern, 0);
    } else {
        pipeline->custom_buffer = av_malloc(32 * 1024);
        if (pipeline->custom_buffer == NULL) {
            kmb_avf_set_error(output_error, "Could not allocate the output callback buffer.");
            return KMB_ALLOCATION_FAILED;
        }
        pipeline->custom_io = avio_alloc_context(
            pipeline->custom_buffer,
            32 * 1024,
            1,
            &pipeline->write_state,
            NULL,
            kmb_avf_write_packet,
            NULL
        );
        if (pipeline->custom_io == NULL) {
            kmb_avf_set_error(output_error, "Could not create the output callback context.");
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
    }
    result = avformat_write_header(pipeline->output, &pipeline->muxer_options);
    if (result < 0) {
        if (pipeline->write_state.cancelled) {
            return KMB_CANCELLED;
        }
        kmb_avf_set_av_error(
            output_error,
            is_mpeg_ts_hls ? "Could not write the MPEG-TS HLS header" : "Could not write the fragmented MP4 header",
            result
        );
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_write_video_packets(KmbAvfPipeline *pipeline, char **output_error) {
    int result = 0;
    while ((result = avcodec_receive_packet(pipeline->video_encoder, pipeline->video_packet)) >= 0) {
        AVStream *output_stream = pipeline->output->streams[pipeline->output_video_track_id];
        if (pipeline->video_packet->duration <= 0 &&
            pipeline->video_encoder->framerate.num > 0 && pipeline->video_encoder->framerate.den > 0) {
            pipeline->video_packet->duration = av_rescale_q(
                1,
                av_inv_q(pipeline->video_encoder->framerate),
                pipeline->video_encoder->time_base
            );
        }
        if (pipeline->progress_callback != NULL) {
            const int64_t packet_time = pipeline->video_packet->pts != AV_NOPTS_VALUE
                ? pipeline->video_packet->pts
                : pipeline->video_packet->dts;
            if (packet_time != AV_NOPTS_VALUE) {
                const int64_t presentation_time_us = av_rescale_q(
                    packet_time,
                    pipeline->video_encoder->time_base,
                    AV_TIME_BASE_Q
                );
                if (pipeline->progress_callback(pipeline->progress_opaque, presentation_time_us) != 0) {
                    pipeline->write_state.cancelled = 1;
                    av_packet_unref(pipeline->video_packet);
                    return KMB_CANCELLED;
                }
            }
        }
        av_packet_rescale_ts(
            pipeline->video_packet,
            pipeline->video_encoder->time_base,
            output_stream->time_base
        );
        pipeline->video_packet->stream_index = pipeline->output_video_track_id;
        pipeline->video_packet->pos = -1;
        result = av_interleaved_write_frame(pipeline->output, pipeline->video_packet);
        av_packet_unref(pipeline->video_packet);
        if (result < 0) {
            if (pipeline->write_state.cancelled) {
                return KMB_CANCELLED;
            }
            kmb_avf_set_av_error(output_error, "Could not write an encoded video packet", result);
            return KMB_WRITE_FAILED;
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_avf_set_av_error(output_error, "Could not receive an encoded video packet", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_write_audio_packets(KmbAvfPipeline *pipeline, char **output_error);

static KmbResult kmb_avf_encode_silent_audio_until(
    KmbAvfPipeline *pipeline,
    int64_t target_audio_pts,
    int drain_partial,
    char **output_error
) {
    const int frame_size = pipeline->audio_encoder->frame_size > 0
        ? pipeline->audio_encoder->frame_size
        : 1024;
    while (pipeline->next_audio_pts < target_audio_pts) {
        AVFrame *frame = NULL;
        int64_t remaining = target_audio_pts - pipeline->next_audio_pts;
        int result = 0;
        KmbResult bridge_result = KMB_OK;
        if (!drain_partial && remaining < frame_size) {
            break;
        }
        frame = av_frame_alloc();
        if (frame == NULL) {
            kmb_avf_set_error(output_error, "Could not allocate a silent AAC input frame.");
            return KMB_ALLOCATION_FAILED;
        }
        frame->nb_samples = frame_size;
        frame->format = pipeline->audio_encoder->sample_fmt;
        frame->sample_rate = pipeline->audio_encoder->sample_rate;
        result = av_channel_layout_copy(&frame->ch_layout, &pipeline->audio_encoder->ch_layout);
        if (result >= 0) {
            result = av_frame_get_buffer(frame, 0);
        }
        if (result >= 0) {
            result = av_samples_set_silence(
                frame->data,
                0,
                frame_size,
                pipeline->audio_encoder->ch_layout.nb_channels,
                pipeline->audio_encoder->sample_fmt
            );
        }
        if (result < 0) {
            av_frame_free(&frame);
            kmb_avf_set_av_error(output_error, "Could not prepare a silent AAC input frame", result);
            return KMB_WRITE_FAILED;
        }
        frame->pts = pipeline->next_audio_pts;
        pipeline->next_audio_pts += frame_size;
        result = avcodec_send_frame(pipeline->audio_encoder, frame);
        av_frame_free(&frame);
        if (result < 0) {
            kmb_avf_set_av_error(output_error, "Could not submit a silent AAC input frame", result);
            return KMB_WRITE_FAILED;
        }
        bridge_result = kmb_avf_write_audio_packets(pipeline, output_error);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
    }
    return KMB_OK;
}

static KmbResult kmb_avf_encode_filtered_video(KmbAvfPipeline *pipeline, char **output_error) {
    int result = 0;
    while ((result = av_buffersink_get_frame(pipeline->video_buffer_sink, pipeline->video_filtered_frame)) >= 0) {
        AVRational sink_time_base = av_buffersink_get_time_base(pipeline->video_buffer_sink);
        int64_t frame_duration = 1;
        KmbResult bridge_result = KMB_OK;
        if (pipeline->video_encoder->framerate.num > 0 &&
            pipeline->video_encoder->framerate.den > 0) {
            frame_duration = av_rescale_q(
                1,
                av_inv_q(pipeline->video_encoder->framerate),
                pipeline->video_encoder->time_base
            );
            if (frame_duration <= 0) {
                frame_duration = 1;
            }
        }
        if (pipeline->video_filtered_frame->pts != AV_NOPTS_VALUE &&
            sink_time_base.num > 0 && sink_time_base.den > 0) {
            pipeline->video_filtered_frame->pts = av_rescale_q(
                pipeline->video_filtered_frame->pts,
                sink_time_base,
                pipeline->video_encoder->time_base
            );
            pipeline->video_filtered_frame->pts = kmb_rebase_timestamp(
                pipeline->video_filtered_frame->pts,
                pipeline->video_encoder->time_base,
                pipeline->requested_start_time_us
            );
        }
        if (!pipeline->video_pts_initialized) {
            if (pipeline->video_filtered_frame->pts == AV_NOPTS_VALUE) {
                pipeline->video_filtered_frame->pts = 0;
            }
            pipeline->video_pts_initialized = 1;
        } else if (pipeline->video_filtered_frame->pts == AV_NOPTS_VALUE ||
                   pipeline->video_filtered_frame->pts < pipeline->next_video_pts - frame_duration) {
            pipeline->video_filtered_frame->pts = pipeline->next_video_pts;
        }
        pipeline->video_filtered_frame->duration = frame_duration;
        pipeline->next_video_pts = pipeline->video_filtered_frame->pts + frame_duration;
        pipeline->video_filtered_frame->pict_type = AV_PICTURE_TYPE_NONE;
        pipeline->video_filtered_frame->color_range = AVCOL_RANGE_MPEG;
        pipeline->video_filtered_frame->color_primaries = AVCOL_PRI_BT709;
        pipeline->video_filtered_frame->color_trc = AVCOL_TRC_BT709;
        pipeline->video_filtered_frame->colorspace = AVCOL_SPC_BT709;
        result = avcodec_send_frame(pipeline->video_encoder, pipeline->video_filtered_frame);
        av_frame_unref(pipeline->video_filtered_frame);
        if (result < 0) {
            kmb_avf_set_av_error(output_error, "Could not submit an SDR video frame", result);
            return KMB_WRITE_FAILED;
        }
        bridge_result = kmb_avf_write_video_packets(pipeline, output_error);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
        if (pipeline->synthesize_silent_audio) {
            int64_t target_audio_pts = av_rescale_q(
                pipeline->next_video_pts,
                pipeline->video_encoder->time_base,
                pipeline->audio_encoder->time_base
            );
            bridge_result = kmb_avf_encode_silent_audio_until(
                pipeline,
                target_audio_pts,
                0,
                output_error
            );
            if (bridge_result != KMB_OK) {
                return bridge_result;
            }
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_avf_set_av_error(output_error, "Could not read an SDR video frame", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_decode_video_packet(
    KmbAvfPipeline *pipeline,
    const AVPacket *packet,
    char **output_error
) {
    AVStream *input_stream = pipeline->input->streams[pipeline->selected_video_track_id];
    int result = avcodec_send_packet(pipeline->video_decoder, packet);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not submit a compressed video packet", result);
        return KMB_READ_FAILED;
    }
    while ((result = avcodec_receive_frame(pipeline->video_decoder, pipeline->video_decoded_frame)) >= 0) {
        KmbResult bridge_result = KMB_OK;
        int64_t source_pts = pipeline->video_decoded_frame->best_effort_timestamp;
        if (kmb_timestamp_precedes_origin(
                source_pts,
                input_stream->time_base,
                pipeline->requested_start_time_us
            )) {
            av_frame_unref(pipeline->video_decoded_frame);
            continue;
        }
        /* Preserve source time through the subtitle filter; rebase only before encoding. */
        pipeline->video_decoded_frame->pts = source_pts;
        result = av_buffersrc_add_frame_flags(
            pipeline->video_buffer_source,
            pipeline->video_decoded_frame,
            AV_BUFFERSRC_FLAG_KEEP_REF
        );
        av_frame_unref(pipeline->video_decoded_frame);
        if (result < 0) {
            kmb_avf_set_av_error(output_error, "Could not submit a decoded video frame", result);
            return KMB_WRITE_FAILED;
        }
        bridge_result = kmb_avf_encode_filtered_video(pipeline, output_error);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_avf_set_av_error(output_error, "Could not decode a video frame", result);
        return KMB_READ_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_write_audio_packets(KmbAvfPipeline *pipeline, char **output_error) {
    int result = 0;
    while ((result = avcodec_receive_packet(pipeline->audio_encoder, pipeline->audio_packet)) >= 0) {
        AVStream *output_stream = pipeline->output->streams[pipeline->output_audio_track_id];
        av_packet_rescale_ts(
            pipeline->audio_packet,
            pipeline->audio_encoder->time_base,
            output_stream->time_base
        );
        pipeline->audio_packet->stream_index = pipeline->output_audio_track_id;
        pipeline->audio_packet->pos = -1;
        result = av_interleaved_write_frame(pipeline->output, pipeline->audio_packet);
        av_packet_unref(pipeline->audio_packet);
        if (result < 0) {
            if (pipeline->write_state.cancelled) {
                return KMB_CANCELLED;
            }
            kmb_avf_set_av_error(output_error, "Could not write an encoded audio packet", result);
            return KMB_WRITE_FAILED;
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_avf_set_av_error(output_error, "Could not receive an encoded audio packet", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_encode_audio_fifo(
    KmbAvfPipeline *pipeline,
    int drain_partial,
    char **output_error
) {
    const int encoder_frame_size = pipeline->audio_encoder->frame_size;
    while ((encoder_frame_size > 0 && av_audio_fifo_size(pipeline->audio_fifo) >= encoder_frame_size) ||
           (drain_partial && av_audio_fifo_size(pipeline->audio_fifo) > 0)) {
        int queued = av_audio_fifo_size(pipeline->audio_fifo);
        int sample_count = encoder_frame_size > 0 ? FFMIN(queued, encoder_frame_size) : queued;
        AVFrame *frame = av_frame_alloc();
        int result = 0;
        KmbResult bridge_result = KMB_OK;
        if (frame == NULL) {
            kmb_avf_set_error(output_error, "Could not allocate an AAC input frame.");
            return KMB_ALLOCATION_FAILED;
        }
        frame->nb_samples = sample_count;
        frame->format = pipeline->audio_encoder->sample_fmt;
        frame->sample_rate = pipeline->audio_encoder->sample_rate;
        result = av_channel_layout_copy(&frame->ch_layout, &pipeline->audio_encoder->ch_layout);
        if (result >= 0) {
            result = av_frame_get_buffer(frame, 0);
        }
        if (result >= 0 && av_audio_fifo_read(pipeline->audio_fifo, (void **)frame->data, sample_count) != sample_count) {
            result = AVERROR(EIO);
        }
        if (result < 0) {
            av_frame_free(&frame);
            kmb_avf_set_av_error(output_error, "Could not prepare an AAC input frame", result);
            return KMB_WRITE_FAILED;
        }
        frame->pts = pipeline->next_audio_pts;
        pipeline->next_audio_pts += sample_count;
        result = avcodec_send_frame(pipeline->audio_encoder, frame);
        av_frame_free(&frame);
        if (result < 0) {
            kmb_avf_set_av_error(output_error, "Could not submit an AAC input frame", result);
            return KMB_WRITE_FAILED;
        }
        bridge_result = kmb_avf_write_audio_packets(pipeline, output_error);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
    }
    return KMB_OK;
}

static KmbResult kmb_avf_queue_converted_audio(
    KmbAvfPipeline *pipeline,
    const AVFrame *decoded,
    char **output_error
) {
    int input_sample_rate = pipeline->audio_decoder->sample_rate;
    int output_capacity = av_rescale_rnd(
        swr_get_delay(pipeline->audio_resampler, input_sample_rate) + decoded->nb_samples,
        pipeline->audio_encoder->sample_rate,
        input_sample_rate,
        AV_ROUND_UP
    );
    AVFrame *converted = av_frame_alloc();
    int converted_samples = 0;
    int result = 0;
    if (converted == NULL) {
        kmb_avf_set_error(output_error, "Could not allocate a converted audio frame.");
        return KMB_ALLOCATION_FAILED;
    }
    converted->nb_samples = output_capacity;
    converted->format = pipeline->audio_encoder->sample_fmt;
    converted->sample_rate = pipeline->audio_encoder->sample_rate;
    result = av_channel_layout_copy(&converted->ch_layout, &pipeline->audio_encoder->ch_layout);
    if (result >= 0) {
        result = av_frame_get_buffer(converted, 0);
    }
    if (result >= 0) {
        converted_samples = swr_convert(
            pipeline->audio_resampler,
            converted->data,
            output_capacity,
            (const uint8_t *const *)decoded->extended_data,
            decoded->nb_samples
        );
        result = converted_samples;
    }
    if (result >= 0) {
        result = av_audio_fifo_realloc(
            pipeline->audio_fifo,
            av_audio_fifo_size(pipeline->audio_fifo) + converted_samples
        );
    }
    if (result >= 0 &&
        av_audio_fifo_write(pipeline->audio_fifo, (void **)converted->data, converted_samples) != converted_samples) {
        result = AVERROR(EIO);
    }
    av_frame_free(&converted);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not convert and queue decoded audio", result);
        return KMB_WRITE_FAILED;
    }
    return kmb_avf_encode_audio_fifo(pipeline, 0, output_error);
}

static KmbResult kmb_avf_decode_audio_packet(
    KmbAvfPipeline *pipeline,
    const AVPacket *packet,
    char **output_error
) {
    AVStream *input_stream = pipeline->input->streams[pipeline->selected_audio_track_id];
    int result = avcodec_send_packet(pipeline->audio_decoder, packet);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not submit a compressed audio packet", result);
        return KMB_READ_FAILED;
    }
    while ((result = avcodec_receive_frame(pipeline->audio_decoder, pipeline->audio_decoded_frame)) >= 0) {
        KmbResult bridge_result = KMB_OK;
        int64_t source_pts = pipeline->audio_decoded_frame->best_effort_timestamp;
        if (kmb_timestamp_precedes_origin(
                source_pts,
                input_stream->time_base,
                pipeline->requested_start_time_us
            )) {
            av_frame_unref(pipeline->audio_decoded_frame);
            continue;
        }
        if (!pipeline->audio_pts_initialized) {
            int64_t rebased_pts = kmb_rebase_timestamp(
                source_pts,
                input_stream->time_base,
                pipeline->requested_start_time_us
            );
            pipeline->next_audio_pts =
                rebased_pts == AV_NOPTS_VALUE
                    ? 0
                    : av_rescale_q(rebased_pts, input_stream->time_base, pipeline->audio_encoder->time_base);
            pipeline->audio_pts_initialized = 1;
        }
        bridge_result = kmb_avf_queue_converted_audio(pipeline, pipeline->audio_decoded_frame, output_error);
        av_frame_unref(pipeline->audio_decoded_frame);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_avf_set_av_error(output_error, "Could not decode an audio frame", result);
        return KMB_READ_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_flush_audio_resampler(KmbAvfPipeline *pipeline, char **output_error) {
    int input_sample_rate = pipeline->audio_decoder->sample_rate;
    for (;;) {
        int output_capacity = av_rescale_rnd(
            swr_get_delay(pipeline->audio_resampler, input_sample_rate),
            pipeline->audio_encoder->sample_rate,
            input_sample_rate,
            AV_ROUND_UP
        );
        AVFrame *converted = NULL;
        int converted_samples = 0;
        int result = 0;
        if (output_capacity <= 0) {
            break;
        }
        converted = av_frame_alloc();
        if (converted == NULL) {
            kmb_avf_set_error(output_error, "Could not allocate a final converted audio frame.");
            return KMB_ALLOCATION_FAILED;
        }
        converted->nb_samples = output_capacity;
        converted->format = pipeline->audio_encoder->sample_fmt;
        converted->sample_rate = pipeline->audio_encoder->sample_rate;
        result = av_channel_layout_copy(&converted->ch_layout, &pipeline->audio_encoder->ch_layout);
        if (result >= 0) {
            result = av_frame_get_buffer(converted, 0);
        }
        if (result >= 0) {
            converted_samples = swr_convert(
                pipeline->audio_resampler,
                converted->data,
                output_capacity,
                NULL,
                0
            );
            result = converted_samples;
        }
        if (result >= 0) {
            result = av_audio_fifo_realloc(
                pipeline->audio_fifo,
                av_audio_fifo_size(pipeline->audio_fifo) + converted_samples
            );
        }
        if (result >= 0 &&
            av_audio_fifo_write(pipeline->audio_fifo, (void **)converted->data, converted_samples) != converted_samples) {
            result = AVERROR(EIO);
        }
        av_frame_free(&converted);
        if (result < 0) {
            kmb_avf_set_av_error(output_error, "Could not flush converted audio", result);
            return KMB_WRITE_FAILED;
        }
        if (converted_samples == 0) {
            break;
        }
    }
    return KMB_OK;
}

static KmbResult kmb_avf_flush(KmbAvfPipeline *pipeline, char **output_error) {
    KmbResult bridge_result = kmb_avf_decode_video_packet(pipeline, NULL, output_error);
    int result = 0;
    if (bridge_result != KMB_OK) {
        return bridge_result;
    }
    result = av_buffersrc_add_frame_flags(pipeline->video_buffer_source, NULL, 0);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not flush the SDR video filter", result);
        return KMB_WRITE_FAILED;
    }
    bridge_result = kmb_avf_encode_filtered_video(pipeline, output_error);
    if (bridge_result != KMB_OK) {
        return bridge_result;
    }
    result = avcodec_send_frame(pipeline->video_encoder, NULL);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not flush the AVC encoder", result);
        return KMB_WRITE_FAILED;
    }
    bridge_result = kmb_avf_write_video_packets(pipeline, output_error);
    if (bridge_result != KMB_OK || pipeline->audio_encoder == NULL) {
        return bridge_result;
    }
    if (pipeline->audio_decoder != NULL) {
        bridge_result = kmb_avf_decode_audio_packet(pipeline, NULL, output_error);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
        bridge_result = kmb_avf_flush_audio_resampler(pipeline, output_error);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
        bridge_result = kmb_avf_encode_audio_fifo(pipeline, 1, output_error);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
    } else if (pipeline->synthesize_silent_audio) {
        int64_t target_audio_pts = av_rescale_q(
            pipeline->next_video_pts,
            pipeline->video_encoder->time_base,
            pipeline->audio_encoder->time_base
        );
        bridge_result = kmb_avf_encode_silent_audio_until(
            pipeline,
            target_audio_pts,
            1,
            output_error
        );
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
    }
    result = avcodec_send_frame(pipeline->audio_encoder, NULL);
    if (result < 0) {
        kmb_avf_set_av_error(output_error, "Could not flush the AAC encoder", result);
        return KMB_WRITE_FAILED;
    }
    return kmb_avf_write_audio_packets(pipeline, output_error);
}

static KmbResult kmb_avf_run(KmbAvfPipeline *pipeline, char **output_error) {
    int result = 0;
    KmbResult bridge_result = KMB_OK;
    while ((result = av_read_frame(pipeline->input, pipeline->input_packet)) >= 0) {
        if (pipeline->input_packet->stream_index == pipeline->selected_video_track_id) {
            bridge_result = kmb_avf_decode_video_packet(pipeline, pipeline->input_packet, output_error);
        } else if (pipeline->input_packet->stream_index == pipeline->selected_audio_track_id) {
            bridge_result = kmb_avf_decode_audio_packet(pipeline, pipeline->input_packet, output_error);
        }
        av_packet_unref(pipeline->input_packet);
        if (bridge_result != KMB_OK) {
            return bridge_result;
        }
    }
    if (result != AVERROR_EOF) {
        kmb_avf_set_av_error(output_error, "Could not read a media packet", result);
        return KMB_READ_FAILED;
    }
    bridge_result = kmb_avf_flush(pipeline, output_error);
    if (bridge_result != KMB_OK) {
        return bridge_result;
    }
    result = av_write_trailer(pipeline->output);
    if (result < 0) {
        if (pipeline->write_state.cancelled) {
            return KMB_CANCELLED;
        }
        kmb_avf_set_av_error(output_error, "Could not finalize compatibility output", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_avf_transcode_internal(
    const char *input_locator,
    int64_t fragment_duration_us,
    int64_t start_time_us,
    int32_t preferred_video_track_id,
    int32_t preferred_audio_track_id,
    int32_t preferred_subtitle_track_id,
    KmbWriteCallback write_callback,
    KmbProgressCallback progress_callback,
    void *callback_opaque,
    const char *hls_playlist_path,
    const char *hls_segment_path_pattern,
    int maximum_playlist_segments,
    char **output_error
) {
    KmbAvfPipeline pipeline = {0};
    KmbResult bridge_result = KMB_OK;
    pipeline.requested_start_time_us = start_time_us;
    pipeline.write_state = (KmbAvfWriteState){write_callback, callback_opaque, 0};
    pipeline.progress_callback = progress_callback;
    pipeline.progress_opaque = callback_opaque;
    bridge_result = kmb_avf_open_input(
        &pipeline,
        input_locator,
        preferred_video_track_id,
        preferred_audio_track_id,
        preferred_subtitle_track_id,
        start_time_us,
        output_error
    );
    if (bridge_result == KMB_OK) {
        pipeline.synthesize_silent_audio =
            hls_playlist_path != NULL &&
            preferred_audio_track_id != -2 &&
            pipeline.selected_audio_track_id < 0;
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_avf_open_decoder(
            &pipeline,
            pipeline.selected_video_track_id,
            &pipeline.video_decoder,
            "video",
            output_error
        );
    }
    if (bridge_result == KMB_OK && pipeline.selected_audio_track_id >= 0) {
        bridge_result = kmb_avf_open_decoder(
            &pipeline,
            pipeline.selected_audio_track_id,
            &pipeline.audio_decoder,
            "audio",
            output_error
        );
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_avf_open_video_encoder(&pipeline, output_error);
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_avf_create_video_filter(&pipeline, input_locator, output_error);
    }
    if (bridge_result == KMB_OK &&
        (pipeline.audio_decoder != NULL || pipeline.synthesize_silent_audio)) {
        bridge_result = kmb_avf_open_audio_encoder(&pipeline, output_error);
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_avf_open_output(
            &pipeline,
            fragment_duration_us,
            hls_playlist_path,
            hls_segment_path_pattern,
            maximum_playlist_segments,
            output_error
        );
    }
    if (bridge_result == KMB_OK) {
        pipeline.input_packet = av_packet_alloc();
        pipeline.video_packet = av_packet_alloc();
        pipeline.video_decoded_frame = av_frame_alloc();
        pipeline.video_filtered_frame = av_frame_alloc();
        if (pipeline.audio_encoder != NULL) {
            pipeline.audio_packet = av_packet_alloc();
        }
        if (pipeline.audio_decoder != NULL) {
            pipeline.audio_decoded_frame = av_frame_alloc();
        }
        if (pipeline.input_packet == NULL || pipeline.video_packet == NULL ||
            pipeline.video_decoded_frame == NULL || pipeline.video_filtered_frame == NULL ||
            (pipeline.audio_encoder != NULL && pipeline.audio_packet == NULL) ||
            (pipeline.audio_decoder != NULL && pipeline.audio_decoded_frame == NULL)) {
            kmb_avf_set_error(output_error, "Could not allocate compatibility conversion frames and packets.");
            bridge_result = KMB_ALLOCATION_FAILED;
        }
    }
    if (bridge_result == KMB_OK) {
        bridge_result = kmb_avf_run(&pipeline, output_error);
    }
    kmb_avf_cleanup(&pipeline);
    return bridge_result;
}

KmbResult kmb_transcode_avfoundation_fragmented_mp4_stream(
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
    if (output_error != NULL) {
        *output_error = NULL;
    }
    if (input_locator == NULL || fragment_duration_us <= 0 || start_time_us < 0 ||
        preferred_subtitle_track_id < -2 || write_callback == NULL) {
        kmb_avf_set_error(output_error, "Valid input, callback, track ids, duration, and start time are required.");
        return KMB_INVALID_ARGUMENT;
    }
    return kmb_avf_transcode_internal(
        input_locator,
        fragment_duration_us,
        start_time_us,
        preferred_video_track_id,
        preferred_audio_track_id,
        preferred_subtitle_track_id,
        write_callback,
        NULL,
        opaque,
        NULL,
        NULL,
        0,
        output_error
    );
}

KmbResult kmb_transcode_cast_mpeg_ts_hls(
    const char *input_locator,
    const char *playlist_path,
    const char *segment_path_pattern,
    int64_t segment_duration_us,
    int32_t maximum_playlist_segments,
    int64_t start_time_us,
    int32_t preferred_video_track_id,
    int32_t preferred_audio_track_id,
    int32_t preferred_subtitle_track_id,
    KmbProgressCallback progress_callback,
    void *opaque,
    char **output_error
) {
    if (output_error != NULL) {
        *output_error = NULL;
    }
    if (input_locator == NULL || playlist_path == NULL || segment_path_pattern == NULL ||
        segment_duration_us <= 0 || maximum_playlist_segments < 3 || start_time_us < 0 ||
        preferred_subtitle_track_id < -2 || progress_callback == NULL) {
        kmb_avf_set_error(
            output_error,
            "Valid input, private HLS paths, callback, track ids, duration, and playlist limit are required."
        );
        return KMB_INVALID_ARGUMENT;
    }
    return kmb_avf_transcode_internal(
        input_locator,
        segment_duration_us,
        start_time_us,
        preferred_video_track_id,
        preferred_audio_track_id,
        preferred_subtitle_track_id,
        NULL,
        progress_callback,
        opaque,
        playlist_path,
        segment_path_pattern,
        maximum_playlist_segments,
        output_error
    );
}

#else

KmbResult kmb_transcode_avfoundation_fragmented_mp4_stream(
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
        *output_error = av_strdup("This runtime does not include AVC/AAC compatibility transcoding.");
    }
    return KMB_UNSUPPORTED;
}

KmbResult kmb_transcode_cast_mpeg_ts_hls(
    const char *input_locator,
    const char *playlist_path,
    const char *segment_path_pattern,
    int64_t segment_duration_us,
    int32_t maximum_playlist_segments,
    int64_t start_time_us,
    int32_t preferred_video_track_id,
    int32_t preferred_audio_track_id,
    int32_t preferred_subtitle_track_id,
    KmbProgressCallback progress_callback,
    void *opaque,
    char **output_error
) {
    (void)input_locator;
    (void)playlist_path;
    (void)segment_path_pattern;
    (void)segment_duration_us;
    (void)maximum_playlist_segments;
    (void)start_time_us;
    (void)preferred_video_track_id;
    (void)preferred_audio_track_id;
    (void)preferred_subtitle_track_id;
    (void)progress_callback;
    (void)opaque;
    if (output_error != NULL) {
        *output_error = av_strdup("This runtime does not include AVC/AAC compatibility transcoding.");
    }
    return KMB_UNSUPPORTED;
}

#endif
