# Relinking the Android runtime

Build replacement shared objects with `native/build-ffmpeg-android.sh`, using
the exact FFmpeg source and patch declared by the artifact manifest. Replace
the five `.so` files for the selected Android ABI without modifying the Kotlin
consumer. Caller-provided runtimes may use a different effective license, but
their redistribution obligations belong to the application distributor.
