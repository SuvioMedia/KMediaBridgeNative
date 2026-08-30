/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "kmedia_bridge.h"

#include <jni.h>
#include <libavcodec/jni.h>
#include <stdint.h>
#include <string.h>

typedef struct KmbAndroidCallback {
    JNIEnv *environment;
    jobject consumer;
    jmethodID accept_method;
} KmbAndroidCallback;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *java_vm, void *reserved) {
    (void)reserved;
    if (java_vm == NULL || av_jni_set_java_vm(java_vm, NULL) < 0) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

static void kmb_android_throw(JNIEnv *environment, const char *message) {
    jclass exception_class = (*environment)->FindClass(environment, "java/lang/IllegalStateException");
    if (exception_class != NULL) {
        (*environment)->ThrowNew(
            environment,
            exception_class,
            message != NULL ? message : "The native media bridge operation failed."
        );
        (*environment)->DeleteLocalRef(environment, exception_class);
    }
}

static jbyteArray kmb_android_bytes(JNIEnv *environment, const char *value) {
    const size_t length = value != NULL ? strlen(value) : 0;
    jbyteArray result;
    if (length > INT32_MAX) {
        kmb_android_throw(environment, "A native media bridge response exceeded the JNI size limit.");
        return NULL;
    }
    result = (*environment)->NewByteArray(environment, (jsize)length);
    if (result != NULL && length > 0) {
        (*environment)->SetByteArrayRegion(
            environment,
            result,
            0,
            (jsize)length,
            (const jbyte *)value
        );
    }
    return result;
}

static int32_t kmb_android_write(void *opaque, const uint8_t *bytes, int32_t size) {
    KmbAndroidCallback *callback = (KmbAndroidCallback *)opaque;
    jbyteArray payload;
    jboolean accepted;
    if (callback == NULL || callback->environment == NULL || callback->consumer == NULL ||
        callback->accept_method == NULL || bytes == NULL || size <= 0) {
        return 1;
    }
    payload = (*callback->environment)->NewByteArray(callback->environment, size);
    if (payload == NULL) return 1;
    (*callback->environment)->SetByteArrayRegion(
        callback->environment,
        payload,
        0,
        size,
        (const jbyte *)bytes
    );
    accepted = (*callback->environment)->CallBooleanMethod(
        callback->environment,
        callback->consumer,
        callback->accept_method,
        payload
    );
    (*callback->environment)->DeleteLocalRef(callback->environment, payload);
    if ((*callback->environment)->ExceptionCheck(callback->environment)) return 1;
    return accepted == JNI_TRUE ? 0 : 1;
}

static int kmb_android_callback_init(
    JNIEnv *environment,
    jobject consumer,
    KmbAndroidCallback *callback
) {
    jclass consumer_class;
    if (consumer == NULL || callback == NULL) {
        kmb_android_throw(environment, "A native media fragment consumer is required.");
        return 0;
    }
    consumer_class = (*environment)->GetObjectClass(environment, consumer);
    if (consumer_class == NULL) return 0;
    callback->accept_method = (*environment)->GetMethodID(environment, consumer_class, "accept", "([B)Z");
    (*environment)->DeleteLocalRef(environment, consumer_class);
    if (callback->accept_method == NULL) return 0;
    callback->environment = environment;
    callback->consumer = consumer;
    return 1;
}

static void kmb_android_finish_call(
    JNIEnv *environment,
    KmbResult result,
    char *error,
    const char *fallback
) {
    if (!(*environment)->ExceptionCheck(environment) && result != KMB_OK && result != KMB_CANCELLED) {
        kmb_android_throw(environment, error != NULL && error[0] != '\0' ? error : fallback);
    }
    kmb_free_string(error);
}

JNIEXPORT jint JNICALL
Java_cc_suviomedia_kmediabridge_ffmpeg_AndroidFfmpegNative_abiVersion(
    JNIEnv *environment,
    jobject receiver
) {
    (void)environment;
    (void)receiver;
    return (jint)kmb_abi_version();
}

JNIEXPORT jbyteArray JNICALL
Java_cc_suviomedia_kmediabridge_ffmpeg_AndroidFfmpegNative_ffmpegVersionBytes(
    JNIEnv *environment,
    jobject receiver
) {
    (void)receiver;
    return kmb_android_bytes(environment, kmb_ffmpeg_version());
}

JNIEXPORT jbyteArray JNICALL
Java_cc_suviomedia_kmediabridge_ffmpeg_AndroidFfmpegNative_ffmpegLicenseBytes(
    JNIEnv *environment,
    jobject receiver
) {
    (void)receiver;
    return kmb_android_bytes(environment, kmb_ffmpeg_license());
}

JNIEXPORT jbyteArray JNICALL
Java_cc_suviomedia_kmediabridge_ffmpeg_AndroidFfmpegNative_ffmpegConfigurationBytes(
    JNIEnv *environment,
    jobject receiver
) {
    (void)receiver;
    return kmb_android_bytes(environment, kmb_ffmpeg_configuration());
}

JNIEXPORT jbyteArray JNICALL
Java_cc_suviomedia_kmediabridge_ffmpeg_AndroidFfmpegNative_runtimeFeaturesJsonBytes(
    JNIEnv *environment,
    jobject receiver
) {
    (void)receiver;
    return kmb_android_bytes(environment, kmb_runtime_features_json());
}

JNIEXPORT jbyteArray JNICALL
Java_cc_suviomedia_kmediabridge_ffmpeg_AndroidFfmpegNative_probeJsonBytes(
    JNIEnv *environment,
    jobject receiver,
    jstring input_locator
) {
    const char *locator = NULL;
    char *json = NULL;
    char *error = NULL;
    KmbResult result;
    jbyteArray response = NULL;
    (void)receiver;
    if (input_locator == NULL) {
        kmb_android_throw(environment, "A media input locator is required.");
        return NULL;
    }
    locator = (*environment)->GetStringUTFChars(environment, input_locator, NULL);
    if (locator == NULL) return NULL;
    result = kmb_probe_json(locator, &json, &error);
    (*environment)->ReleaseStringUTFChars(environment, input_locator, locator);
    if (result == KMB_OK) response = kmb_android_bytes(environment, json);
    kmb_free_string(json);
    kmb_android_finish_call(environment, result, error, "The native media probe failed.");
    return response;
}

JNIEXPORT jint JNICALL
Java_cc_suviomedia_kmediabridge_ffmpeg_AndroidFfmpegNative_remuxFragmentedMp4Stream(
    JNIEnv *environment,
    jobject receiver,
    jstring input_locator,
    jlong fragment_duration_us,
    jlong start_time_us,
    jint preferred_video_track_id,
    jint preferred_audio_track_id,
    jobject consumer
) {
    const char *locator = NULL;
    char *error = NULL;
    KmbAndroidCallback callback = {0};
    KmbResult result;
    (void)receiver;
    if (input_locator == NULL || !kmb_android_callback_init(environment, consumer, &callback)) return KMB_INVALID_ARGUMENT;
    locator = (*environment)->GetStringUTFChars(environment, input_locator, NULL);
    if (locator == NULL) return KMB_ALLOCATION_FAILED;
    result = kmb_remux_fragmented_mp4_stream(
        locator,
        fragment_duration_us,
        start_time_us,
        preferred_video_track_id,
        preferred_audio_track_id,
        kmb_android_write,
        &callback,
        &error
    );
    (*environment)->ReleaseStringUTFChars(environment, input_locator, locator);
    kmb_android_finish_call(environment, result, error, "The native fragmented MP4 remux failed.");
    return result;
}

JNIEXPORT jint JNICALL
Java_cc_suviomedia_kmediabridge_ffmpeg_AndroidFfmpegNative_toneMapHdrToSdrFragmentedMp4Stream(
    JNIEnv *environment,
    jobject receiver,
    jstring input_locator,
    jlong fragment_duration_us,
    jlong start_time_us,
    jint preferred_video_track_id,
    jint preferred_audio_track_id,
    jobject consumer
) {
    const char *locator = NULL;
    char *error = NULL;
    KmbAndroidCallback callback = {0};
    KmbResult result;
    (void)receiver;
    if (input_locator == NULL || !kmb_android_callback_init(environment, consumer, &callback)) return KMB_INVALID_ARGUMENT;
    locator = (*environment)->GetStringUTFChars(environment, input_locator, NULL);
    if (locator == NULL) return KMB_ALLOCATION_FAILED;
    result = kmb_tone_map_hdr_to_sdr_fragmented_mp4_stream(
        locator,
        fragment_duration_us,
        start_time_us,
        preferred_video_track_id,
        preferred_audio_track_id,
        kmb_android_write,
        &callback,
        &error
    );
    (*environment)->ReleaseStringUTFChars(environment, input_locator, locator);
    kmb_android_finish_call(environment, result, error, "The controlled HDR-to-SDR conversion failed.");
    return result;
}
