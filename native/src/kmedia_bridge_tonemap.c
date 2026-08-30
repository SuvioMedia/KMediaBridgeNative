/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "kmedia_bridge.h"

#include <libavutil/avstring.h>
#include <libavutil/mem.h>

#if defined(KMB_ENABLE_HDR_TO_SDR)

#include "kmedia_bridge_avc_encoder.h"
#include "kmedia_bridge_hdr_math.h"
#include "kmedia_bridge_timestamps.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#define KMB_TONEMAP_TRACE(message) \
    ((void)__android_log_write(ANDROID_LOG_DEBUG, "KMediaBridge", (message)))
#else
#define KMB_TONEMAP_TRACE(message) ((void)0)
#endif

#define KMB_TONEMAP_IO_BUFFER_SIZE (32 * 1024)
#define KMB_TONEMAP_TARGET_WHITE_NITS 100.0
#define KMB_TONEMAP_DEFAULT_SOURCE_PEAK_NITS 1000.0
#define KMB_TONEMAP_MAX_SOURCE_PEAK_NITS 10000.0
#define KMB_CODEC_EAGAIN_RETRY_LIMIT 5000
#define KMB_CODEC_EAGAIN_RETRY_DELAY_US 1000

typedef struct KmbToneMapWriteState {
    KmbWriteCallback callback;
    void *opaque;
    int cancelled;
    int traced_output;
} KmbToneMapWriteState;

typedef struct KmbToneMapPipeline {
    AVFormatContext *input;
    AVFormatContext *output;
    AVCodecContext *decoder;
    AVCodecContext *encoder;
    AVPacket *input_packet;
    AVPacket *encoded_packet;
    AVFrame *decoded_frame;
    AVFrame *linear_frame;
    AVFrame *sdr_frame;
    SwsContext *decode_to_float;
    SwsContext *float_to_sdr;
    KmbHdrColorTransform *color_transform;
    AVIOContext *custom_io;
    unsigned char *custom_buffer;
    AVDictionary *muxer_options;
    int selected_video_track_id;
    int selected_audio_track_id;
    int output_video_track_id;
    int output_audio_track_id;
    int conversion_width;
    int conversion_height;
    enum AVPixelFormat conversion_input_format;
    enum AVColorRange conversion_input_range;
    enum AVColorTransferCharacteristic source_transfer;
    KmbTimestampState audio_timestamp_state;
    KmbToneMapWriteState write_state;
    int64_t requested_start_time_us;
    int64_t last_video_pts;
    int64_t fallback_video_pts;
    int encoded_frame_count;
    int traced_input_packet;
    int traced_decoded_frame;
    int traced_converted_frame;
    int traced_encoder_input;
    int traced_encoder_accepted;
    int traced_encoded_packet;
} KmbToneMapPipeline;

static void kmb_tonemap_set_error(char **output_error, const char *message) {
    if (output_error != NULL) {
        *output_error = av_strdup(message != NULL ? message : "Unknown HDR-to-SDR pipeline error.");
    }
}

static void kmb_tonemap_set_av_error(char **output_error, const char *operation, int error_code) {
    char error_text[AV_ERROR_MAX_STRING_SIZE] = {0};
    char combined[256] = {0};
    av_strerror(error_code, error_text, sizeof(error_text));
    av_strlcpy(combined, operation, sizeof(combined));
    av_strlcat(combined, ": ", sizeof(combined));
    av_strlcat(combined, error_text, sizeof(combined));
    kmb_tonemap_set_error(output_error, combined);
}

static int kmb_tonemap_select_track(
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
    for (index = 0; index < input->nb_streams; ++index) {
        const AVStream *stream = input->streams[index];
        if (stream->codecpar->codec_type != type) continue;
        if (first < 0) first = (int)index;
        if ((stream->disposition & AV_DISPOSITION_DEFAULT) != 0) return (int)index;
    }
    return first;
}

static int kmb_tonemap_write_packet(void *opaque, const uint8_t *bytes, int size) {
    KmbToneMapWriteState *state = (KmbToneMapWriteState *)opaque;
    if (state == NULL || state->callback == NULL || size < 0) return AVERROR(EINVAL);
    if (!state->traced_output) {
        state->traced_output = 1;
        KMB_TONEMAP_TRACE("tone-map emitted its first MP4 bytes");
    }
    if (state->callback(state->opaque, bytes, size) != 0) {
        state->cancelled = 1;
        return AVERROR_EXIT;
    }
    return size;
}

static void kmb_tonemap_reset_conversion(KmbToneMapPipeline *pipeline) {
    sws_freeContext(pipeline->decode_to_float);
    sws_freeContext(pipeline->float_to_sdr);
    pipeline->decode_to_float = NULL;
    pipeline->float_to_sdr = NULL;
    av_frame_free(&pipeline->linear_frame);
    av_frame_free(&pipeline->sdr_frame);
    pipeline->conversion_width = 0;
    pipeline->conversion_height = 0;
    pipeline->conversion_input_format = AV_PIX_FMT_NONE;
    pipeline->conversion_input_range = AVCOL_RANGE_UNSPECIFIED;
}

static void kmb_tonemap_cleanup(KmbToneMapPipeline *pipeline) {
    av_dict_free(&pipeline->muxer_options);
    av_packet_free(&pipeline->input_packet);
    av_packet_free(&pipeline->encoded_packet);
    av_frame_free(&pipeline->decoded_frame);
    kmb_tonemap_reset_conversion(pipeline);
    kmb_hdr_color_transform_free(&pipeline->color_transform);
    avcodec_free_context(&pipeline->decoder);
    avcodec_free_context(&pipeline->encoder);
    if (pipeline->custom_io != NULL) {
        if (pipeline->output != NULL) pipeline->output->pb = NULL;
        avio_context_free(&pipeline->custom_io);
    }
    av_freep(&pipeline->custom_buffer);
    avformat_free_context(pipeline->output);
    avformat_close_input(&pipeline->input);
}

static int kmb_tonemap_has_dolby_vision(const AVCodecParameters *parameters) {
    return av_packet_side_data_get(
        parameters->coded_side_data,
        parameters->nb_coded_side_data,
        AV_PKT_DATA_DOVI_CONF
    ) != NULL;
}

static KmbResult kmb_tonemap_validate_stream(
    const AVCodecParameters *parameters,
    char **output_error
) {
    if (kmb_tonemap_has_dolby_vision(parameters)) {
        kmb_tonemap_set_error(
            output_error,
            "Dolby Vision must be handled by the dedicated profile-aware converter before HDR-to-SDR tone mapping."
        );
        return KMB_UNSUPPORTED;
    }
    if (parameters->color_trc != AVCOL_TRC_SMPTE2084 &&
        parameters->color_trc != AVCOL_TRC_ARIB_STD_B67) {
        kmb_tonemap_set_error(output_error, "The selected video track is not explicitly tagged as PQ or HLG.");
        return KMB_UNSUPPORTED;
    }
    if (parameters->color_primaries != AVCOL_PRI_BT2020) {
        kmb_tonemap_set_error(output_error, "The selected HDR video track is not explicitly tagged with BT.2020 primaries.");
        return KMB_UNSUPPORTED;
    }
    if (parameters->color_space != AVCOL_SPC_BT2020_NCL) {
        kmb_tonemap_set_error(
            output_error,
            "The selected HDR video track must use the supported BT.2020 non-constant-luminance matrix."
        );
        return KMB_UNSUPPORTED;
    }
    if (parameters->color_range != AVCOL_RANGE_MPEG && parameters->color_range != AVCOL_RANGE_JPEG) {
        kmb_tonemap_set_error(output_error, "The selected HDR video track has no unambiguous limited or full color range.");
        return KMB_UNSUPPORTED;
    }
    if (parameters->width <= 0 || parameters->height <= 0 ||
        (parameters->width & 1) != 0 || (parameters->height & 1) != 0) {
        kmb_tonemap_set_error(output_error, "The selected HDR video dimensions cannot be represented by AVC 4:2:0 output.");
        return KMB_UNSUPPORTED;
    }
    return KMB_OK;
}

static KmbResult kmb_tonemap_open_input(
    KmbToneMapPipeline *pipeline,
    const char *input_locator,
    int preferred_video_track_id,
    int preferred_audio_track_id,
    char **output_error
) {
    int result = avformat_open_input(&pipeline->input, input_locator, NULL, NULL);
    KmbResult validation = KMB_OK;
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not open media input", result);
        return KMB_OPEN_INPUT_FAILED;
    }
    result = avformat_find_stream_info(pipeline->input, NULL);
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not read media stream information", result);
        return KMB_STREAM_INFO_FAILED;
    }
    pipeline->selected_video_track_id =
        kmb_tonemap_select_track(pipeline->input, AVMEDIA_TYPE_VIDEO, preferred_video_track_id);
    pipeline->selected_audio_track_id = -1;
    if (preferred_audio_track_id != -2) {
        pipeline->selected_audio_track_id =
            kmb_tonemap_select_track(pipeline->input, AVMEDIA_TYPE_AUDIO, preferred_audio_track_id);
    }
    if (pipeline->selected_video_track_id < 0 || pipeline->selected_audio_track_id == -2) {
        kmb_tonemap_set_error(output_error, "A requested HDR video or audio track is unavailable.");
        return KMB_UNSUPPORTED;
    }
    validation = kmb_tonemap_validate_stream(
        pipeline->input->streams[pipeline->selected_video_track_id]->codecpar,
        output_error
    );
    if (validation != KMB_OK) return validation;
    pipeline->source_transfer =
        pipeline->input->streams[pipeline->selected_video_track_id]->codecpar->color_trc;
    if (pipeline->requested_start_time_us > 0) {
        result = avformat_seek_file(
            pipeline->input,
            -1,
            INT64_MIN,
            pipeline->requested_start_time_us,
            pipeline->requested_start_time_us,
            AVSEEK_FLAG_BACKWARD
        );
        if (result < 0) {
            kmb_tonemap_set_av_error(output_error, "Could not seek media input", result);
            return KMB_READ_FAILED;
        }
        avformat_flush(pipeline->input);
    }
    return KMB_OK;
}

static const char *kmb_tonemap_android_decoder_name(enum AVCodecID codec_id) {
#if defined(__ANDROID__)
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return "h264_mediacodec";
        case AV_CODEC_ID_HEVC:
            return "hevc_mediacodec";
        case AV_CODEC_ID_VP9:
            return "vp9_mediacodec";
        case AV_CODEC_ID_AV1:
            return "av1_mediacodec";
        default:
            return NULL;
    }
#else
    (void)codec_id;
    return NULL;
#endif
}

static int kmb_tonemap_try_decoder(
    KmbToneMapPipeline *pipeline,
    AVStream *stream,
    const AVCodec *codec
) {
    AVCodecContext *context = NULL;
    int result = 0;
    if (codec == NULL) return AVERROR_DECODER_NOT_FOUND;
    context = avcodec_alloc_context3(codec);
    if (context == NULL) return AVERROR(ENOMEM);
    result = avcodec_parameters_to_context(context, stream->codecpar);
    if (result >= 0) {
        context->pkt_timebase = stream->time_base;
        result = avcodec_open2(context, codec, NULL);
    }
    if (result < 0) {
        avcodec_free_context(&context);
        return result;
    }
    pipeline->decoder = context;
    return 0;
}

static KmbResult kmb_tonemap_open_decoder(KmbToneMapPipeline *pipeline, char **output_error) {
    AVStream *stream = pipeline->input->streams[pipeline->selected_video_track_id];
    const char *hardware_name = kmb_tonemap_android_decoder_name(stream->codecpar->codec_id);
    const AVCodec *software = avcodec_find_decoder(stream->codecpar->codec_id);
    int result = AVERROR_DECODER_NOT_FOUND;
    if (hardware_name != NULL) {
        result = kmb_tonemap_try_decoder(pipeline, stream, avcodec_find_decoder_by_name(hardware_name));
    }
    if (pipeline->decoder == NULL) {
        result = kmb_tonemap_try_decoder(pipeline, stream, software);
    }
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not open an HDR video decoder", result);
        return result == AVERROR(ENOMEM) ? KMB_ALLOCATION_FAILED : KMB_UNSUPPORTED;
    }
    KMB_TONEMAP_TRACE("tone-map decoder ready");
    return KMB_OK;
}

static const char *kmb_tonemap_android_compatibility_encoder(void) {
#if defined(__ANDROID__)
    char manufacturer[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.product.manufacturer", manufacturer) > 0 &&
        av_strcasecmp(manufacturer, "NVIDIA") == 0) {
        return "OMX.google.h264.encoder";
    }
#endif
    return NULL;
}

static KmbResult kmb_tonemap_open_encoder(KmbToneMapPipeline *pipeline, char **output_error) {
    KmbAvcEncoderAttempt attempts[8] = {0};
    AVStream *input_stream = pipeline->input->streams[pipeline->selected_video_track_id];
    AVRational frame_rate = av_guess_frame_rate(pipeline->input, input_stream, NULL);
    double frames_per_second = 30.0;
    int64_t bitrate = 0;
    int attempt_count = kmb_avc_encoder_attempts(attempts, 8);
    int attempt_index = 0;
    int encoder_name_attempt = 0;
    int found_encoder = 0;
    int last_result = AVERROR_ENCODER_NOT_FOUND;
    const char *compatibility_encoder = kmb_tonemap_android_compatibility_encoder();
    const int encoder_name_attempt_count = compatibility_encoder != NULL ? 2 : 1;
    if (attempt_count == 0) {
        kmb_tonemap_set_error(output_error, "No AVC encoder is available in the selected runtime.");
        return KMB_UNSUPPORTED;
    }
    if (frame_rate.num <= 0 || frame_rate.den <= 0) frame_rate = (AVRational){30, 1};
    frames_per_second = av_q2d(frame_rate);
    if (!isfinite(frames_per_second) || frames_per_second <= 0.0 || frames_per_second > 240.0) {
        frame_rate = (AVRational){30, 1};
        frames_per_second = 30.0;
    }
    bitrate = (int64_t)(input_stream->codecpar->width * (int64_t)input_stream->codecpar->height *
        frames_per_second / 8.0);
    if (bitrate < 2000000) bitrate = 2000000;
    if (bitrate > 24000000) bitrate = 24000000;
    for (attempt_index = 0; attempt_index < attempt_count; ++attempt_index) {
        const KmbAvcEncoderAttempt *attempt = &attempts[attempt_index];
        const AVCodec *codec = kmb_avc_encoder_find(attempt);
        if (codec == NULL) continue;
        found_encoder = 1;
        for (encoder_name_attempt = 0;
             encoder_name_attempt < encoder_name_attempt_count;
             ++encoder_name_attempt) {
            const char *encoder_name =
                compatibility_encoder != NULL && encoder_name_attempt == 0
                    ? compatibility_encoder
                    : NULL;
            AVCodecContext *context = avcodec_alloc_context3(codec);
            AVDictionary *options = NULL;
            if (context == NULL) {
                kmb_tonemap_set_error(output_error, "Could not allocate the AVC encoder.");
                return KMB_ALLOCATION_FAILED;
            }
            context->width = input_stream->codecpar->width;
            context->height = input_stream->codecpar->height;
            context->sample_aspect_ratio = input_stream->codecpar->sample_aspect_ratio;
            if (context->sample_aspect_ratio.num <= 0 || context->sample_aspect_ratio.den <= 0) {
                context->sample_aspect_ratio = (AVRational){1, 1};
            }
            context->pix_fmt = attempt->pixel_format;
            context->time_base = input_stream->time_base;
            if (context->time_base.num <= 0 || context->time_base.den <= 0) {
                context->time_base = (AVRational){1, 90000};
            }
            context->framerate = frame_rate;
            context->bit_rate = bitrate;
            context->gop_size = (int)(frames_per_second * 2.0 + 0.5);
            if (context->gop_size < 12) context->gop_size = 12;
            context->max_b_frames = 0;
            context->color_range = AVCOL_RANGE_MPEG;
            context->color_primaries = AVCOL_PRI_BT709;
            context->color_trc = AVCOL_TRC_BT709;
            context->colorspace = AVCOL_SPC_BT709;
            /*
             * Android MediaCodec encoders, including the official ARM emulator and NVIDIA
             * Shield, do not reliably resume after FFmpeg generates global AVC headers with
             * a dummy frame followed by MediaCodec.flush(). The fragmented MP4 muxer uses
             * delay_moov and derives avcC from the first real packet, so Android deliberately
             * avoids that dummy cycle for every selected hardware encoder.
             */
#if !defined(__ANDROID__)
            context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#endif
            kmb_avc_encoder_apply_options(attempt, &options);
            if (encoder_name != NULL) {
                av_dict_set(&options, "codec_name", encoder_name, 0);
            }
            last_result = avcodec_open2(context, codec, &options);
            av_dict_free(&options);
            if (last_result >= 0) {
                pipeline->encoder = context;
                if (encoder_name != NULL) {
                    KMB_TONEMAP_TRACE("tone-map selected the Android compatibility encoder");
                }
                KMB_TONEMAP_TRACE("tone-map encoder ready");
                return KMB_OK;
            }
            avcodec_free_context(&context);
        }
    }
    if (!found_encoder) {
        kmb_tonemap_set_error(output_error, "No AVC encoder is available in the selected runtime.");
        return KMB_UNSUPPORTED;
    }
    kmb_tonemap_set_av_error(output_error, "Could not open an AVC encoder", last_result);
    return KMB_UNSUPPORTED;
}

static KmbResult kmb_tonemap_open_output(
    KmbToneMapPipeline *pipeline,
    int64_t fragment_duration_us,
    char **output_error
) {
    AVStream *video_output = NULL;
    int result = avformat_alloc_output_context2(&pipeline->output, NULL, "mp4", NULL);
    if (result < 0 || pipeline->output == NULL) {
        kmb_tonemap_set_av_error(output_error, "Could not create fragmented MP4 output", result);
        return KMB_OPEN_OUTPUT_FAILED;
    }
    video_output = avformat_new_stream(pipeline->output, NULL);
    if (video_output == NULL) {
        kmb_tonemap_set_error(output_error, "Could not create the tone-mapped video output stream.");
        return KMB_ALLOCATION_FAILED;
    }
    pipeline->output_video_track_id = video_output->index;
    video_output->time_base = pipeline->encoder->time_base;
    result = avcodec_parameters_from_context(video_output->codecpar, pipeline->encoder);
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not publish AVC encoder parameters", result);
        return KMB_OPEN_OUTPUT_FAILED;
    }
    video_output->codecpar->codec_tag = MKTAG('a', 'v', 'c', '1');
    video_output->codecpar->color_range = AVCOL_RANGE_MPEG;
    video_output->codecpar->color_primaries = AVCOL_PRI_BT709;
    video_output->codecpar->color_trc = AVCOL_TRC_BT709;
    video_output->codecpar->color_space = AVCOL_SPC_BT709;
    pipeline->output_audio_track_id = -1;
    if (pipeline->selected_audio_track_id >= 0) {
        AVStream *audio_input = pipeline->input->streams[pipeline->selected_audio_track_id];
        AVStream *audio_output = NULL;
        if (avformat_query_codec(
                pipeline->output->oformat,
                audio_input->codecpar->codec_id,
                FF_COMPLIANCE_NORMAL
            ) <= 0) {
            kmb_tonemap_set_error(output_error, "The selected audio track cannot be copied into fragmented MP4.");
            return KMB_UNSUPPORTED;
        }
        audio_output = avformat_new_stream(pipeline->output, NULL);
        if (audio_output == NULL) {
            kmb_tonemap_set_error(output_error, "Could not create the copied audio output stream.");
            return KMB_ALLOCATION_FAILED;
        }
        pipeline->output_audio_track_id = audio_output->index;
        audio_output->time_base = audio_input->time_base;
        result = avcodec_parameters_copy(audio_output->codecpar, audio_input->codecpar);
        if (result < 0) {
            kmb_tonemap_set_av_error(output_error, "Could not copy audio stream parameters", result);
            return KMB_OPEN_OUTPUT_FAILED;
        }
        audio_output->codecpar->codec_tag = 0;
    }
    pipeline->custom_buffer = av_malloc(KMB_TONEMAP_IO_BUFFER_SIZE);
    if (pipeline->custom_buffer == NULL) {
        kmb_tonemap_set_error(output_error, "Could not allocate the output callback buffer.");
        return KMB_ALLOCATION_FAILED;
    }
    pipeline->custom_io = avio_alloc_context(
        pipeline->custom_buffer,
        KMB_TONEMAP_IO_BUFFER_SIZE,
        1,
        &pipeline->write_state,
        NULL,
        kmb_tonemap_write_packet,
        NULL
    );
    if (pipeline->custom_io == NULL) {
        kmb_tonemap_set_error(output_error, "Could not create the output callback context.");
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
        if (pipeline->write_state.cancelled) return KMB_CANCELLED;
        kmb_tonemap_set_av_error(output_error, "Could not write the fragmented MP4 header", result);
        return KMB_WRITE_FAILED;
    }
    KMB_TONEMAP_TRACE("tone-map fragmented MP4 muxer ready");
    return KMB_OK;
}

static enum AVColorTransferCharacteristic kmb_tonemap_frame_transfer(
    const KmbToneMapPipeline *pipeline,
    const AVFrame *frame
) {
    return frame->color_trc == AVCOL_TRC_UNSPECIFIED ? pipeline->source_transfer : frame->color_trc;
}

static enum AVColorRange kmb_tonemap_frame_range(
    const KmbToneMapPipeline *pipeline,
    const AVFrame *frame
) {
    if (frame->color_range == AVCOL_RANGE_MPEG || frame->color_range == AVCOL_RANGE_JPEG) {
        return frame->color_range;
    }
    return pipeline->input->streams[pipeline->selected_video_track_id]->codecpar->color_range;
}

static double kmb_tonemap_peak_from_side_data(
    const AVFrameSideData *content_light_side_data,
    const AVFrameSideData *mastering_side_data
) {
    double peak = 0.0;
    if (content_light_side_data != NULL &&
        content_light_side_data->size >= sizeof(AVContentLightMetadata)) {
        const AVContentLightMetadata *light =
            (const AVContentLightMetadata *)content_light_side_data->data;
        if (light->MaxCLL > 0) peak = light->MaxCLL;
    }
    if (peak <= 0.0 && mastering_side_data != NULL &&
        mastering_side_data->size >= sizeof(AVMasteringDisplayMetadata)) {
        const AVMasteringDisplayMetadata *mastering =
            (const AVMasteringDisplayMetadata *)mastering_side_data->data;
        if (mastering->has_luminance) peak = av_q2d(mastering->max_luminance);
    }
    return peak;
}

static double kmb_tonemap_source_peak_nits(
    const KmbToneMapPipeline *pipeline,
    const AVFrame *frame
) {
    const AVCodecParameters *parameters =
        pipeline->input->streams[pipeline->selected_video_track_id]->codecpar;
    double peak = kmb_tonemap_peak_from_side_data(
        av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL),
        av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)
    );
    if (peak <= 0.0) {
        const AVPacketSideData *light = av_packet_side_data_get(
            parameters->coded_side_data,
            parameters->nb_coded_side_data,
            AV_PKT_DATA_CONTENT_LIGHT_LEVEL
        );
        const AVPacketSideData *mastering = av_packet_side_data_get(
            parameters->coded_side_data,
            parameters->nb_coded_side_data,
            AV_PKT_DATA_MASTERING_DISPLAY_METADATA
        );
        if (light != NULL && light->size >= sizeof(AVContentLightMetadata)) {
            const AVContentLightMetadata *metadata = (const AVContentLightMetadata *)light->data;
            if (metadata->MaxCLL > 0) peak = metadata->MaxCLL;
        }
        if (peak <= 0.0 && mastering != NULL && mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
            const AVMasteringDisplayMetadata *metadata = (const AVMasteringDisplayMetadata *)mastering->data;
            if (metadata->has_luminance) peak = av_q2d(metadata->max_luminance);
        }
    }
    if (!isfinite(peak) || peak <= 0.0) peak = KMB_TONEMAP_DEFAULT_SOURCE_PEAK_NITS;
    if (peak < KMB_TONEMAP_TARGET_WHITE_NITS) peak = KMB_TONEMAP_TARGET_WHITE_NITS;
    if (peak > KMB_TONEMAP_MAX_SOURCE_PEAK_NITS) peak = KMB_TONEMAP_MAX_SOURCE_PEAK_NITS;
    return peak;
}

static KmbResult kmb_tonemap_ensure_color_transform(
    KmbToneMapPipeline *pipeline,
    const AVFrame *frame,
    char **output_error
) {
    const enum AVColorTransferCharacteristic transfer = kmb_tonemap_frame_transfer(pipeline, frame);
    KmbHdrTransfer color_transfer;
    if (transfer != AVCOL_TRC_SMPTE2084 && transfer != AVCOL_TRC_ARIB_STD_B67) {
        kmb_tonemap_set_error(output_error, "A decoded frame changed to an unsupported or ambiguous color transfer.");
        return KMB_UNSUPPORTED;
    }
    if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED && frame->color_primaries != AVCOL_PRI_BT2020) {
        kmb_tonemap_set_error(output_error, "A decoded frame changed away from BT.2020 primaries.");
        return KMB_UNSUPPORTED;
    }
    if (frame->colorspace != AVCOL_SPC_UNSPECIFIED && frame->colorspace != AVCOL_SPC_BT2020_NCL) {
        kmb_tonemap_set_error(output_error, "A decoded frame changed away from BT.2020 non-constant-luminance.");
        return KMB_UNSUPPORTED;
    }
    if (pipeline->color_transform != NULL) {
        if (transfer != pipeline->source_transfer) {
            kmb_tonemap_set_error(output_error, "The decoded stream changed between PQ and HLG without a track boundary.");
            return KMB_UNSUPPORTED;
        }
        return KMB_OK;
    }
    pipeline->source_transfer = transfer;
    color_transfer =
        transfer == AVCOL_TRC_SMPTE2084 ? KMB_HDR_TRANSFER_PQ : KMB_HDR_TRANSFER_HLG;
    pipeline->color_transform = kmb_hdr_color_transform_create(
        color_transfer,
        kmb_tonemap_source_peak_nits(pipeline, frame),
        KMB_TONEMAP_TARGET_WHITE_NITS
    );
    if (pipeline->color_transform == NULL) {
        kmb_tonemap_set_error(output_error, "Could not allocate the controlled HDR color transform.");
        return KMB_ALLOCATION_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_tonemap_allocate_conversion(
    KmbToneMapPipeline *pipeline,
    const AVFrame *frame,
    char **output_error
) {
    const enum AVPixelFormat format = (enum AVPixelFormat)frame->format;
    const enum AVPixelFormat output_format = pipeline->encoder->pix_fmt;
    const enum AVColorRange range = kmb_tonemap_frame_range(pipeline, frame);
    const int *bt2020_coefficients = sws_getCoefficients(SWS_CS_BT2020);
    const int *bt709_coefficients = sws_getCoefficients(SWS_CS_ITU709);
    int result = 0;
    if (frame->width != pipeline->encoder->width || frame->height != pipeline->encoder->height) {
        kmb_tonemap_set_error(output_error, "A decoded frame changed dimensions after the AVC output was configured.");
        return KMB_UNSUPPORTED;
    }
    if (range != AVCOL_RANGE_MPEG && range != AVCOL_RANGE_JPEG) {
        kmb_tonemap_set_error(output_error, "A decoded HDR frame has no unambiguous limited or full color range.");
        return KMB_UNSUPPORTED;
    }
    if (!sws_isSupportedInput(format)) {
        kmb_tonemap_set_error(output_error, "The decoded HDR pixel format cannot be read by the controlled color converter.");
        return KMB_UNSUPPORTED;
    }
    if (output_format != AV_PIX_FMT_YUV420P && output_format != AV_PIX_FMT_NV12) {
        kmb_tonemap_set_error(output_error, "The selected AVC encoder requires an unsupported pixel format.");
        return KMB_UNSUPPORTED;
    }
    if (pipeline->linear_frame != NULL &&
        pipeline->conversion_width == frame->width &&
        pipeline->conversion_height == frame->height &&
        pipeline->conversion_input_format == format &&
        pipeline->conversion_input_range == range) {
        return KMB_OK;
    }
    kmb_tonemap_reset_conversion(pipeline);
    pipeline->linear_frame = av_frame_alloc();
    pipeline->sdr_frame = av_frame_alloc();
    if (pipeline->linear_frame == NULL || pipeline->sdr_frame == NULL) {
        kmb_tonemap_set_error(output_error, "Could not allocate HDR conversion frames.");
        return KMB_ALLOCATION_FAILED;
    }
    pipeline->linear_frame->format = AV_PIX_FMT_GBRPF32;
    pipeline->linear_frame->width = frame->width;
    pipeline->linear_frame->height = frame->height;
    pipeline->linear_frame->color_range = AVCOL_RANGE_JPEG;
    pipeline->linear_frame->color_primaries = AVCOL_PRI_BT2020;
    pipeline->linear_frame->color_trc = kmb_tonemap_frame_transfer(pipeline, frame);
    pipeline->linear_frame->colorspace = AVCOL_SPC_RGB;
    pipeline->sdr_frame->format = output_format;
    pipeline->sdr_frame->width = frame->width;
    pipeline->sdr_frame->height = frame->height;
    pipeline->sdr_frame->color_range = AVCOL_RANGE_MPEG;
    pipeline->sdr_frame->color_primaries = AVCOL_PRI_BT709;
    pipeline->sdr_frame->color_trc = AVCOL_TRC_BT709;
    pipeline->sdr_frame->colorspace = AVCOL_SPC_BT709;
    result = av_frame_get_buffer(pipeline->linear_frame, 32);
    if (result >= 0) result = av_frame_get_buffer(pipeline->sdr_frame, 32);
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not allocate HDR conversion pixel buffers", result);
        return KMB_ALLOCATION_FAILED;
    }
    pipeline->decode_to_float = sws_getContext(
        frame->width,
        frame->height,
        format,
        frame->width,
        frame->height,
        AV_PIX_FMT_GBRPF32,
        SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP,
        NULL,
        NULL,
        NULL
    );
    pipeline->float_to_sdr = sws_getContext(
        frame->width,
        frame->height,
        AV_PIX_FMT_GBRPF32,
        frame->width,
        frame->height,
        output_format,
        SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ERROR_DIFFUSION,
        NULL,
        NULL,
        NULL
    );
    if (pipeline->decode_to_float == NULL || pipeline->float_to_sdr == NULL) {
        kmb_tonemap_set_error(output_error, "Could not configure the HDR pixel conversion contexts.");
        return KMB_UNSUPPORTED;
    }
    result = sws_setColorspaceDetails(
        pipeline->decode_to_float,
        bt2020_coefficients,
        range == AVCOL_RANGE_JPEG,
        bt2020_coefficients,
        1,
        0,
        1 << 16,
        1 << 16
    );
    if (result >= 0) {
        result = sws_setColorspaceDetails(
            pipeline->float_to_sdr,
            bt709_coefficients,
            1,
            bt709_coefficients,
            0,
            0,
            1 << 16,
            1 << 16
        );
    }
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not apply explicit BT.2020-to-BT.709 range conversion", result);
        return KMB_UNSUPPORTED;
    }
    pipeline->conversion_width = frame->width;
    pipeline->conversion_height = frame->height;
    pipeline->conversion_input_format = format;
    pipeline->conversion_input_range = range;
    return KMB_OK;
}

static int64_t kmb_tonemap_frame_pts(
    KmbToneMapPipeline *pipeline,
    const AVFrame *decoded,
    int *should_drop
) {
    AVStream *stream = pipeline->input->streams[pipeline->selected_video_track_id];
    int64_t timestamp = decoded->best_effort_timestamp;
    int64_t timestamp_us = AV_NOPTS_VALUE;
    int64_t encoded_pts = AV_NOPTS_VALUE;
    int64_t duration = av_rescale_q(1, av_inv_q(pipeline->encoder->framerate), pipeline->encoder->time_base);
    if (duration <= 0) duration = 1;
    *should_drop = 0;
    if (timestamp != AV_NOPTS_VALUE) {
        timestamp_us = av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q);
        if (timestamp_us < pipeline->requested_start_time_us) {
            *should_drop = 1;
            return AV_NOPTS_VALUE;
        }
        encoded_pts = av_rescale_q(timestamp, stream->time_base, pipeline->encoder->time_base);
    }
    if (encoded_pts == AV_NOPTS_VALUE) encoded_pts = pipeline->fallback_video_pts;
    if (pipeline->last_video_pts != AV_NOPTS_VALUE && encoded_pts <= pipeline->last_video_pts) {
        encoded_pts = pipeline->last_video_pts + duration;
    }
    pipeline->last_video_pts = encoded_pts;
    pipeline->fallback_video_pts = encoded_pts + duration;
    return encoded_pts;
}

static KmbResult kmb_tonemap_convert_frame(
    KmbToneMapPipeline *pipeline,
    const AVFrame *decoded,
    AVFrame **output,
    char **output_error
) {
    KmbResult bridge_result = kmb_tonemap_ensure_color_transform(pipeline, decoded, output_error);
    int scaled_rows = 0;
    int result = 0;
    int drop = 0;
    int64_t output_pts = AV_NOPTS_VALUE;
    if (bridge_result != KMB_OK) return bridge_result;
    bridge_result = kmb_tonemap_allocate_conversion(pipeline, decoded, output_error);
    if (bridge_result != KMB_OK) return bridge_result;
    output_pts = kmb_tonemap_frame_pts(pipeline, decoded, &drop);
    if (drop) {
        *output = NULL;
        return KMB_OK;
    }
    result = av_frame_make_writable(pipeline->linear_frame);
    if (result >= 0) result = av_frame_make_writable(pipeline->sdr_frame);
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not make HDR conversion buffers writable", result);
        return KMB_ALLOCATION_FAILED;
    }
    scaled_rows = sws_scale(
        pipeline->decode_to_float,
        (const uint8_t *const *)decoded->data,
        decoded->linesize,
        0,
        decoded->height,
        pipeline->linear_frame->data,
        pipeline->linear_frame->linesize
    );
    if (scaled_rows != decoded->height) {
        kmb_tonemap_set_error(output_error, "Could not convert the decoded HDR frame to float BT.2020.");
        return KMB_WRITE_FAILED;
    }
    if (!kmb_hdr_color_transform_gbrpf32(
            pipeline->color_transform,
            (float *)pipeline->linear_frame->data[0],
            pipeline->linear_frame->linesize[0],
            (float *)pipeline->linear_frame->data[1],
            pipeline->linear_frame->linesize[1],
            (float *)pipeline->linear_frame->data[2],
            pipeline->linear_frame->linesize[2],
            decoded->width,
            decoded->height
        )) {
        kmb_tonemap_set_error(output_error, "The controlled HDR color transform rejected a decoded frame.");
        return KMB_WRITE_FAILED;
    }
    scaled_rows = sws_scale(
        pipeline->float_to_sdr,
        (const uint8_t *const *)pipeline->linear_frame->data,
        pipeline->linear_frame->linesize,
        0,
        decoded->height,
        pipeline->sdr_frame->data,
        pipeline->sdr_frame->linesize
    );
    if (scaled_rows != decoded->height) {
        kmb_tonemap_set_error(output_error, "Could not quantize the tone-mapped frame to BT.709 AVC input.");
        return KMB_WRITE_FAILED;
    }
    pipeline->sdr_frame->pts = output_pts;
    pipeline->sdr_frame->duration =
        decoded->duration > 0
            ? av_rescale_q(decoded->duration, pipeline->input->streams[pipeline->selected_video_track_id]->time_base,
                pipeline->encoder->time_base)
            : 0;
    pipeline->sdr_frame->sample_aspect_ratio = pipeline->encoder->sample_aspect_ratio;
    pipeline->sdr_frame->pict_type = pipeline->encoded_frame_count == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (pipeline->encoded_frame_count == 0) {
        pipeline->sdr_frame->flags |= AV_FRAME_FLAG_KEY;
    } else {
        pipeline->sdr_frame->flags &= ~AV_FRAME_FLAG_KEY;
    }
    pipeline->sdr_frame->color_range = AVCOL_RANGE_MPEG;
    pipeline->sdr_frame->color_primaries = AVCOL_PRI_BT709;
    pipeline->sdr_frame->color_trc = AVCOL_TRC_BT709;
    pipeline->sdr_frame->colorspace = AVCOL_SPC_BT709;
    if (!pipeline->traced_converted_frame) {
        pipeline->traced_converted_frame = 1;
        KMB_TONEMAP_TRACE("tone-map converted its first frame to BT.709");
    }
    *output = pipeline->sdr_frame;
    return KMB_OK;
}

static KmbResult kmb_tonemap_write_encoded_packets(KmbToneMapPipeline *pipeline, char **output_error) {
    int result = 0;
    while ((result = avcodec_receive_packet(pipeline->encoder, pipeline->encoded_packet)) >= 0) {
        AVStream *output_stream = pipeline->output->streams[pipeline->output_video_track_id];
        if (!pipeline->traced_encoded_packet) {
            pipeline->traced_encoded_packet = 1;
            KMB_TONEMAP_TRACE("tone-map received its first AVC packet");
        }
        if (pipeline->encoded_packet->duration <= 0) {
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
            if (pipeline->write_state.cancelled) return KMB_CANCELLED;
            kmb_tonemap_set_av_error(output_error, "Could not write a tone-mapped video packet", result);
            return KMB_WRITE_FAILED;
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_tonemap_set_av_error(output_error, "Could not receive a tone-mapped video packet", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_tonemap_encode_frame(
    KmbToneMapPipeline *pipeline,
    AVFrame *frame,
    char **output_error
) {
    if (frame != NULL && !pipeline->traced_encoder_input) {
        pipeline->traced_encoder_input = 1;
        KMB_TONEMAP_TRACE("tone-map submitted its first BT.709 frame to the encoder");
    }
    int result = avcodec_send_frame(pipeline->encoder, frame);
    int retry_count = 0;
    while (result == AVERROR(EAGAIN) && retry_count++ < KMB_CODEC_EAGAIN_RETRY_LIMIT) {
        KmbResult bridge_result = kmb_tonemap_write_encoded_packets(pipeline, output_error);
        if (bridge_result != KMB_OK) return bridge_result;
        if (pipeline->write_state.cancelled) return KMB_CANCELLED;
        av_usleep(KMB_CODEC_EAGAIN_RETRY_DELAY_US);
        result = avcodec_send_frame(pipeline->encoder, frame);
    }
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not submit a tone-mapped frame to the AVC encoder", result);
        return KMB_WRITE_FAILED;
    }
    if (frame != NULL && !pipeline->traced_encoder_accepted) {
        pipeline->traced_encoder_accepted = 1;
        KMB_TONEMAP_TRACE("tone-map encoder accepted its first BT.709 frame");
    }
    if (frame != NULL) pipeline->encoded_frame_count++;
    return kmb_tonemap_write_encoded_packets(pipeline, output_error);
}

static KmbResult kmb_tonemap_receive_decoded_frames(
    KmbToneMapPipeline *pipeline,
    char **output_error
) {
    int result = 0;
    while ((result = avcodec_receive_frame(pipeline->decoder, pipeline->decoded_frame)) >= 0) {
        AVFrame *converted = NULL;
        if (!pipeline->traced_decoded_frame) {
            pipeline->traced_decoded_frame = 1;
            KMB_TONEMAP_TRACE("tone-map received its first decoded HDR frame");
        }
        KmbResult bridge_result = kmb_tonemap_convert_frame(
            pipeline,
            pipeline->decoded_frame,
            &converted,
            output_error
        );
        av_frame_unref(pipeline->decoded_frame);
        if (bridge_result != KMB_OK) return bridge_result;
        if (converted != NULL) {
            bridge_result = kmb_tonemap_encode_frame(pipeline, converted, output_error);
            if (bridge_result != KMB_OK) return bridge_result;
        }
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        kmb_tonemap_set_av_error(output_error, "Could not decode an HDR video frame", result);
        return KMB_READ_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_tonemap_decode_packet(
    KmbToneMapPipeline *pipeline,
    const AVPacket *packet,
    char **output_error
) {
    int result = avcodec_send_packet(pipeline->decoder, packet);
    int retry_count = 0;
    while (result == AVERROR(EAGAIN) && retry_count++ < KMB_CODEC_EAGAIN_RETRY_LIMIT) {
        KmbResult bridge_result = kmb_tonemap_receive_decoded_frames(pipeline, output_error);
        if (bridge_result != KMB_OK) return bridge_result;
        if (pipeline->write_state.cancelled) return KMB_CANCELLED;
        av_usleep(KMB_CODEC_EAGAIN_RETRY_DELAY_US);
        result = avcodec_send_packet(pipeline->decoder, packet);
    }
    if (result < 0) {
        kmb_tonemap_set_av_error(output_error, "Could not submit a compressed HDR video packet", result);
        return KMB_READ_FAILED;
    }
    return kmb_tonemap_receive_decoded_frames(pipeline, output_error);
}

static KmbResult kmb_tonemap_copy_audio_packet(
    KmbToneMapPipeline *pipeline,
    AVPacket *packet,
    char **output_error
) {
    AVStream *input_stream = pipeline->input->streams[pipeline->selected_audio_track_id];
    AVStream *output_stream = pipeline->output->streams[pipeline->output_audio_track_id];
    int64_t timestamp = AV_NOPTS_VALUE;
    int result = 0;
    kmb_prepare_packet_timestamps(input_stream, packet, &pipeline->audio_timestamp_state);
    timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (timestamp != AV_NOPTS_VALUE &&
        av_rescale_q(timestamp, input_stream->time_base, AV_TIME_BASE_Q) < pipeline->requested_start_time_us) {
        return KMB_OK;
    }
    av_packet_rescale_ts(packet, input_stream->time_base, output_stream->time_base);
    packet->stream_index = pipeline->output_audio_track_id;
    packet->pos = -1;
    result = av_interleaved_write_frame(pipeline->output, packet);
    if (result < 0) {
        if (pipeline->write_state.cancelled) return KMB_CANCELLED;
        kmb_tonemap_set_av_error(output_error, "Could not write a copied audio packet", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

static KmbResult kmb_tonemap_flush(KmbToneMapPipeline *pipeline, char **output_error) {
    KmbResult bridge_result = kmb_tonemap_decode_packet(pipeline, NULL, output_error);
    if (bridge_result != KMB_OK) return bridge_result;
    return kmb_tonemap_encode_frame(pipeline, NULL, output_error);
}

static KmbResult kmb_tonemap_run(KmbToneMapPipeline *pipeline, char **output_error) {
    int result = 0;
    KmbResult bridge_result = KMB_OK;
    while ((result = av_read_frame(pipeline->input, pipeline->input_packet)) >= 0) {
        if (pipeline->input_packet->stream_index == pipeline->selected_video_track_id) {
            if (!pipeline->traced_input_packet) {
                pipeline->traced_input_packet = 1;
                KMB_TONEMAP_TRACE("tone-map read its first compressed video packet");
            }
            bridge_result = kmb_tonemap_decode_packet(pipeline, pipeline->input_packet, output_error);
        } else if (pipeline->input_packet->stream_index == pipeline->selected_audio_track_id) {
            bridge_result = kmb_tonemap_copy_audio_packet(pipeline, pipeline->input_packet, output_error);
        }
        av_packet_unref(pipeline->input_packet);
        if (bridge_result != KMB_OK) return bridge_result;
    }
    if (result != AVERROR_EOF) {
        kmb_tonemap_set_av_error(output_error, "Could not read a media packet", result);
        return KMB_READ_FAILED;
    }
    bridge_result = kmb_tonemap_flush(pipeline, output_error);
    if (bridge_result != KMB_OK) return bridge_result;
    result = av_write_trailer(pipeline->output);
    if (result < 0) {
        if (pipeline->write_state.cancelled) return KMB_CANCELLED;
        kmb_tonemap_set_av_error(output_error, "Could not finalize tone-mapped fragmented MP4 output", result);
        return KMB_WRITE_FAILED;
    }
    return KMB_OK;
}

KmbResult kmb_tone_map_hdr_to_sdr_fragmented_mp4_stream(
    const char *input_locator,
    int64_t fragment_duration_us,
    int64_t start_time_us,
    int32_t preferred_video_track_id,
    int32_t preferred_audio_track_id,
    KmbWriteCallback write_callback,
    void *opaque,
    char **output_error
) {
    KmbToneMapPipeline pipeline = {0};
    KmbResult bridge_result = KMB_OK;
    KMB_TONEMAP_TRACE("tone-map session started");
    if (output_error != NULL) *output_error = NULL;
    if (input_locator == NULL || fragment_duration_us <= 0 || start_time_us < 0 || write_callback == NULL) {
        kmb_tonemap_set_error(output_error, "Valid input, callback, fragment duration, and start time are required.");
        return KMB_INVALID_ARGUMENT;
    }
    pipeline.write_state = (KmbToneMapWriteState){write_callback, opaque, 0, 0};
    pipeline.requested_start_time_us = start_time_us;
    pipeline.last_video_pts = AV_NOPTS_VALUE;
    pipeline.fallback_video_pts = av_rescale_q(start_time_us, AV_TIME_BASE_Q, (AVRational){1, 90000});
    pipeline.conversion_input_format = AV_PIX_FMT_NONE;
    pipeline.conversion_input_range = AVCOL_RANGE_UNSPECIFIED;
    bridge_result = kmb_tonemap_open_input(
        &pipeline,
        input_locator,
        preferred_video_track_id,
        preferred_audio_track_id,
        output_error
    );
    if (bridge_result == KMB_OK) KMB_TONEMAP_TRACE("tone-map input ready");
    if (bridge_result == KMB_OK) bridge_result = kmb_tonemap_open_decoder(&pipeline, output_error);
    if (bridge_result == KMB_OK) bridge_result = kmb_tonemap_open_encoder(&pipeline, output_error);
    if (bridge_result == KMB_OK) {
        pipeline.fallback_video_pts = av_rescale_q(
            start_time_us,
            AV_TIME_BASE_Q,
            pipeline.encoder->time_base
        );
        bridge_result = kmb_tonemap_open_output(&pipeline, fragment_duration_us, output_error);
    }
    if (bridge_result == KMB_OK) {
        pipeline.input_packet = av_packet_alloc();
        pipeline.encoded_packet = av_packet_alloc();
        pipeline.decoded_frame = av_frame_alloc();
        if (pipeline.input_packet == NULL || pipeline.encoded_packet == NULL || pipeline.decoded_frame == NULL) {
            kmb_tonemap_set_error(output_error, "Could not allocate HDR transcoding frames and packets.");
            bridge_result = KMB_ALLOCATION_FAILED;
        }
    }
    if (bridge_result == KMB_OK) bridge_result = kmb_tonemap_run(&pipeline, output_error);
    if (bridge_result == KMB_OK) KMB_TONEMAP_TRACE("tone-map session completed");
    kmb_tonemap_cleanup(&pipeline);
    return bridge_result;
}

#else

KmbResult kmb_tone_map_hdr_to_sdr_fragmented_mp4_stream(
    const char *input_locator,
    int64_t fragment_duration_us,
    int64_t start_time_us,
    int32_t preferred_video_track_id,
    int32_t preferred_audio_track_id,
    KmbWriteCallback write_callback,
    void *opaque,
    char **output_error
) {
    (void)input_locator;
    (void)fragment_duration_us;
    (void)start_time_us;
    (void)preferred_video_track_id;
    (void)preferred_audio_track_id;
    (void)write_callback;
    (void)opaque;
    if (output_error != NULL) {
        *output_error = av_strdup("This runtime does not include the optional controlled HDR-to-SDR pipeline.");
    }
    return KMB_UNSUPPORTED;
}

#endif
