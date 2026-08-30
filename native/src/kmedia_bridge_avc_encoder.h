/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef KMEDIA_BRIDGE_AVC_ENCODER_H
#define KMEDIA_BRIDGE_AVC_ENCODER_H

#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/pixfmt.h>

typedef enum KmbAvcEncoderPlatform {
    KMB_AVC_ENCODER_PLATFORM_ANDROID = 0,
    KMB_AVC_ENCODER_PLATFORM_MACOS = 1,
    KMB_AVC_ENCODER_PLATFORM_WINDOWS = 2,
    KMB_AVC_ENCODER_PLATFORM_LINUX = 3,
    KMB_AVC_ENCODER_PLATFORM_OTHER = 4,
} KmbAvcEncoderPlatform;

typedef enum KmbAvcEncoderMode {
    KMB_AVC_ENCODER_HARDWARE_DEFAULT = 0,
    KMB_AVC_ENCODER_VIDEOTOOLBOX_HARDWARE = 1,
    KMB_AVC_ENCODER_VIDEOTOOLBOX_FALLBACK = 2,
    KMB_AVC_ENCODER_MEDIA_FOUNDATION_HARDWARE = 3,
    KMB_AVC_ENCODER_MEDIA_FOUNDATION_FALLBACK = 4,
    KMB_AVC_ENCODER_SOFTWARE = 5,
} KmbAvcEncoderMode;

typedef struct KmbAvcEncoderAttempt {
    const char *codec_name;
    enum AVPixelFormat pixel_format;
    KmbAvcEncoderMode mode;
} KmbAvcEncoderAttempt;

static inline KmbAvcEncoderPlatform kmb_avc_encoder_current_platform(void) {
#if defined(__ANDROID__)
    return KMB_AVC_ENCODER_PLATFORM_ANDROID;
#elif defined(__APPLE__)
    return KMB_AVC_ENCODER_PLATFORM_MACOS;
#elif defined(_WIN32)
    return KMB_AVC_ENCODER_PLATFORM_WINDOWS;
#elif defined(__linux__)
    return KMB_AVC_ENCODER_PLATFORM_LINUX;
#else
    return KMB_AVC_ENCODER_PLATFORM_OTHER;
#endif
}

static inline void kmb_avc_encoder_append_attempt(
    KmbAvcEncoderAttempt *attempts,
    int capacity,
    int *count,
    const char *codec_name,
    enum AVPixelFormat pixel_format,
    KmbAvcEncoderMode mode
) {
    if (*count >= capacity) return;
    attempts[*count].codec_name = codec_name;
    attempts[*count].pixel_format = pixel_format;
    attempts[*count].mode = mode;
    *count += 1;
}

/*
 * Hardware attempts always precede software-capable attempts. A null codec name is the final
 * runtime-defined AVC fallback and is intentionally last so it cannot shadow an accelerated
 * encoder that the audited runtime exposes explicitly.
 */
static inline int kmb_avc_encoder_attempts_for_platform(
    KmbAvcEncoderPlatform platform,
    KmbAvcEncoderAttempt *attempts,
    int capacity
) {
    int count = 0;
    if (attempts == NULL || capacity <= 0) return 0;
    switch (platform) {
        case KMB_AVC_ENCODER_PLATFORM_ANDROID:
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_mediacodec",
                AV_PIX_FMT_NV12,
                KMB_AVC_ENCODER_HARDWARE_DEFAULT
            );
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_mediacodec",
                AV_PIX_FMT_YUV420P,
                KMB_AVC_ENCODER_HARDWARE_DEFAULT
            );
            break;
        case KMB_AVC_ENCODER_PLATFORM_MACOS:
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_videotoolbox",
                AV_PIX_FMT_YUV420P,
                KMB_AVC_ENCODER_VIDEOTOOLBOX_HARDWARE
            );
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_videotoolbox",
                AV_PIX_FMT_NV12,
                KMB_AVC_ENCODER_VIDEOTOOLBOX_HARDWARE
            );
            break;
        case KMB_AVC_ENCODER_PLATFORM_WINDOWS:
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_mf",
                AV_PIX_FMT_NV12,
                KMB_AVC_ENCODER_MEDIA_FOUNDATION_HARDWARE
            );
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_mf",
                AV_PIX_FMT_YUV420P,
                KMB_AVC_ENCODER_MEDIA_FOUNDATION_HARDWARE
            );
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_nvenc",
                AV_PIX_FMT_NV12,
                KMB_AVC_ENCODER_HARDWARE_DEFAULT
            );
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_nvenc",
                AV_PIX_FMT_YUV420P,
                KMB_AVC_ENCODER_HARDWARE_DEFAULT
            );
            break;
        case KMB_AVC_ENCODER_PLATFORM_LINUX:
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_nvenc",
                AV_PIX_FMT_NV12,
                KMB_AVC_ENCODER_HARDWARE_DEFAULT
            );
            kmb_avc_encoder_append_attempt(
                attempts,
                capacity,
                &count,
                "h264_nvenc",
                AV_PIX_FMT_YUV420P,
                KMB_AVC_ENCODER_HARDWARE_DEFAULT
            );
            break;
        case KMB_AVC_ENCODER_PLATFORM_OTHER:
            break;
    }
    if (platform == KMB_AVC_ENCODER_PLATFORM_MACOS) {
        kmb_avc_encoder_append_attempt(
            attempts,
            capacity,
            &count,
            "h264_videotoolbox",
            AV_PIX_FMT_YUV420P,
            KMB_AVC_ENCODER_VIDEOTOOLBOX_FALLBACK
        );
    } else if (platform == KMB_AVC_ENCODER_PLATFORM_WINDOWS) {
        kmb_avc_encoder_append_attempt(
            attempts,
            capacity,
            &count,
            "h264_mf",
            AV_PIX_FMT_YUV420P,
            KMB_AVC_ENCODER_MEDIA_FOUNDATION_FALLBACK
        );
    }
    kmb_avc_encoder_append_attempt(
        attempts,
        capacity,
        &count,
        "libopenh264",
        AV_PIX_FMT_YUV420P,
        KMB_AVC_ENCODER_SOFTWARE
    );
    kmb_avc_encoder_append_attempt(
        attempts,
        capacity,
        &count,
        NULL,
        AV_PIX_FMT_YUV420P,
        KMB_AVC_ENCODER_SOFTWARE
    );
    return count;
}

static inline int kmb_avc_encoder_attempts(KmbAvcEncoderAttempt *attempts, int capacity) {
    return kmb_avc_encoder_attempts_for_platform(
        kmb_avc_encoder_current_platform(),
        attempts,
        capacity
    );
}

static inline const AVCodec *kmb_avc_encoder_find(const KmbAvcEncoderAttempt *attempt) {
    return attempt->codec_name != NULL
        ? avcodec_find_encoder_by_name(attempt->codec_name)
        : avcodec_find_encoder(AV_CODEC_ID_H264);
}

static inline int kmb_avc_encoder_attempt_is_hardware(const KmbAvcEncoderAttempt *attempt) {
    return attempt->mode == KMB_AVC_ENCODER_HARDWARE_DEFAULT ||
        attempt->mode == KMB_AVC_ENCODER_VIDEOTOOLBOX_HARDWARE ||
        attempt->mode == KMB_AVC_ENCODER_MEDIA_FOUNDATION_HARDWARE;
}

static inline void kmb_avc_encoder_apply_options(
    const KmbAvcEncoderAttempt *attempt,
    AVDictionary **options
) {
    switch (attempt->mode) {
        case KMB_AVC_ENCODER_VIDEOTOOLBOX_HARDWARE:
            av_dict_set(options, "allow_sw", "0", 0);
            av_dict_set(options, "realtime", "1", 0);
            break;
        case KMB_AVC_ENCODER_VIDEOTOOLBOX_FALLBACK:
            av_dict_set(options, "allow_sw", "1", 0);
            av_dict_set(options, "realtime", "1", 0);
            break;
        case KMB_AVC_ENCODER_MEDIA_FOUNDATION_HARDWARE:
            av_dict_set(options, "hw_encoding", "1", 0);
            av_dict_set(options, "scenario", "live_streaming", 0);
            break;
        case KMB_AVC_ENCODER_MEDIA_FOUNDATION_FALLBACK:
            av_dict_set(options, "hw_encoding", "0", 0);
            av_dict_set(options, "scenario", "live_streaming", 0);
            break;
        case KMB_AVC_ENCODER_HARDWARE_DEFAULT:
        case KMB_AVC_ENCODER_SOFTWARE:
            break;
    }
}

#endif
