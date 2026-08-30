# Third-party notices

## Shared FFmpeg and subtitle runtime

KMediaBridge contains only its bridge implementation. Native client artifacts
link dynamically to the exact `KMediaFfmpegRuntime` release declared in their
metadata; they do not redistribute a private FFmpeg or subtitle stack.

`KMediaFfmpegRuntime` publishes FFmpeg 9.0.1, libass 0.17.5, FreeType 2.14.1,
FriBidi 1.0.16 and HarfBuzz 12.2.0 under their respective upstream licenses.
Its release contains the corresponding sources, patches, build arguments,
notices, SBOM and checksums. The exact source-archive URL and SHA-256 are copied
from that runtime release into every KMediaBridge client manifest.

- Runtime project: https://github.com/Shusek/KMediaFfmpegRuntime
- FFmpeg legal information: https://ffmpeg.org/legal.html

No FFmpeg or other upstream trademark rights are granted by this project.

## Java Native Access (JNA)

The desktop JVM backend uses JNA to call the narrow native C ABI. JNA is
offered under `Apache-2.0 OR LGPL-2.1-or-later`; KMediaBridge elects the
Apache-2.0 option. JNA is not part of the shared native runtime artifact.

- Project: https://github.com/java-native-access/jna
- Artifact: `net.java.dev.jna:jna:5.19.1`

## Kotlin and kotlinx.coroutines

Kotlin and kotlinx.coroutines are distributed under the Apache License 2.0.
They are build/runtime dependencies and are not copied into Kotlin/Native
source distributions by this repository except as described by generated
Gradle metadata.

## Gradle Wrapper

The repository includes the unmodified Gradle 9.6.1 wrapper JAR so builds can
bootstrap a pinned Gradle distribution. Gradle is licensed under the Apache
License 2.0. Its license is reproduced at `gradle/wrapper/LICENSE`.

- Project: https://github.com/gradle/gradle
- Pinned source: https://github.com/gradle/gradle/tree/v9.6.1
- Wrapper JAR SHA-256: `497c8c2a7e5031f6aa847f88104aa80a93532ec32ee17bdb8d1d2f67a194a9c7`
