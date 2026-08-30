/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "kmedia_bridge_avc_encoder.h"

#include <assert.h>
#include <string.h>

static void verify_windows_hardware_first(void) {
    KmbAvcEncoderAttempt attempts[8] = {0};
    const int count = kmb_avc_encoder_attempts_for_platform(
        KMB_AVC_ENCODER_PLATFORM_WINDOWS,
        attempts,
        8
    );
    int index = 0;
    assert(count == 7);
    assert(strcmp(attempts[0].codec_name, "h264_mf") == 0);
    assert(attempts[0].pixel_format == AV_PIX_FMT_NV12);
    assert(attempts[0].mode == KMB_AVC_ENCODER_MEDIA_FOUNDATION_HARDWARE);
    assert(strcmp(attempts[2].codec_name, "h264_nvenc") == 0);
    assert(attempts[4].mode == KMB_AVC_ENCODER_MEDIA_FOUNDATION_FALLBACK);
    for (index = 0; index < 4; ++index) {
        assert(kmb_avc_encoder_attempt_is_hardware(&attempts[index]));
    }
    for (index = 4; index < count; ++index) {
        assert(!kmb_avc_encoder_attempt_is_hardware(&attempts[index]));
    }
    assert(attempts[count - 1].codec_name == NULL);
}

static void verify_platform_encoder_order(void) {
    KmbAvcEncoderAttempt attempts[8] = {0};
    int count = kmb_avc_encoder_attempts_for_platform(
        KMB_AVC_ENCODER_PLATFORM_MACOS,
        attempts,
        8
    );
    assert(count == 5);
    assert(strcmp(attempts[0].codec_name, "h264_videotoolbox") == 0);
    assert(attempts[0].mode == KMB_AVC_ENCODER_VIDEOTOOLBOX_HARDWARE);
    assert(attempts[2].mode == KMB_AVC_ENCODER_VIDEOTOOLBOX_FALLBACK);

    count = kmb_avc_encoder_attempts_for_platform(
        KMB_AVC_ENCODER_PLATFORM_ANDROID,
        attempts,
        8
    );
    assert(count == 4);
    assert(strcmp(attempts[0].codec_name, "h264_mediacodec") == 0);
    assert(attempts[0].pixel_format == AV_PIX_FMT_NV12);

    count = kmb_avc_encoder_attempts_for_platform(
        KMB_AVC_ENCODER_PLATFORM_LINUX,
        attempts,
        8
    );
    assert(count == 4);
    assert(strcmp(attempts[0].codec_name, "h264_nvenc") == 0);
    assert(strcmp(attempts[2].codec_name, "libopenh264") == 0);
}

static void verify_backend_options(void) {
    KmbAvcEncoderAttempt attempt = {
        .codec_name = "h264_mf",
        .pixel_format = AV_PIX_FMT_NV12,
        .mode = KMB_AVC_ENCODER_MEDIA_FOUNDATION_HARDWARE,
    };
    AVDictionary *options = NULL;
    const AVDictionaryEntry *entry = NULL;
    kmb_avc_encoder_apply_options(&attempt, &options);
    entry = av_dict_get(options, "hw_encoding", NULL, 0);
    assert(entry != NULL && strcmp(entry->value, "1") == 0);
    entry = av_dict_get(options, "scenario", NULL, 0);
    assert(entry != NULL && strcmp(entry->value, "live_streaming") == 0);
    av_dict_free(&options);

    attempt.mode = KMB_AVC_ENCODER_VIDEOTOOLBOX_HARDWARE;
    kmb_avc_encoder_apply_options(&attempt, &options);
    entry = av_dict_get(options, "allow_sw", NULL, 0);
    assert(entry != NULL && strcmp(entry->value, "0") == 0);
    av_dict_free(&options);

    attempt.mode = KMB_AVC_ENCODER_VIDEOTOOLBOX_FALLBACK;
    kmb_avc_encoder_apply_options(&attempt, &options);
    entry = av_dict_get(options, "allow_sw", NULL, 0);
    assert(entry != NULL && strcmp(entry->value, "1") == 0);
    av_dict_free(&options);
}

int main(void) {
    verify_windows_hardware_first();
    verify_platform_encoder_order();
    verify_backend_options();
    return 0;
}
