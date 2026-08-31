# KMediaBridgeNative

Public LGPL native runtime boundary for KMediaBridge.

The repository contains the C ABI, Android JNI entry points, CMake project,
tests, and Maven payload modules. The proprietary Kotlin API and FFmpeg backend
live in the private KMediaBridge repository.

Published artifacts:

- `cc.suviomedia:kmedia-bridge-native-android`
- `cc.suviomedia:kmedia-bridge-native-desktop`

Both depend on the matching public `cc.suviomedia:kmedia-ffmpeg-runtime-*`
artifact. They do not depend on a private Maven coordinate.

To build the native library against an extracted KMediaFfmpegRuntime SDK:

```shell
PKG_CONFIG_PATH=<runtime-sdk>/lib/pkgconfig \
cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

For Android, pass the Android NDK toolchain and enable the JNI entry points:

```shell
PKG_CONFIG_PATH=<runtime-sdk>/lib/pkgconfig \
cmake -S native -B build/native-android \
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=23 \
  -DKMB_ENABLE_ANDROID_JNI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-android
```

Run `./gradlew verifyAll` to validate the project.
