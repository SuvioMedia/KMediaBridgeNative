// SPDX-License-Identifier: LGPL-2.1-or-later

plugins {
    base
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.vanniktech.maven.publish) apply false
}

val publicationVersion = providers.gradleProperty("publicationVersion").orElse("1.0.0-SNAPSHOT")

allprojects {
    group = "cc.suviomedia"
    version = publicationVersion.get()
}

val verifyPublicBoundary =
    tasks.register("verifyPublicBoundary") {
        group = "verification"
        description = "Rejects proprietary source and incomplete LGPL notices in the public bridge runtime."
        notCompatibleWithConfigurationCache("Reads and audits the complete public source boundary.")
        val auditedFiles =
            fileTree(layout.projectDirectory) {
                exclude(".git/**", ".gradle/**", "**/build/**")
                include(
                    "native/**/*.c",
                    "native/**/*.h",
                    "native/**/CMakeLists.txt",
                    "ffmpeg-runtime-android/build.gradle.kts",
                    "ffmpeg-runtime-desktop/build.gradle.kts",
                )
            }
        inputs.files(auditedFiles)
        inputs.files("LICENSE", "NOTICE", "LICENSES/LGPL-2.1-or-later.txt")

        doLast {
            require(auditedFiles.files.isNotEmpty()) { "The native bridge source inventory is empty." }
            val proprietary =
                auditedFiles.files.filter { file ->
                    "LicenseRef-KMediaBridge-Internal" in file.readText()
                }
            require(proprietary.isEmpty()) {
                "The public bridge runtime contains proprietary material: ${proprietary.sorted()}"
            }
            val nativeSources =
                fileTree("native") {
                    include("**/*.c", "**/*.h", "**/CMakeLists.txt")
                }.files
            val missingSpdx =
                nativeSources.filter { file ->
                    "SPDX-License-Identifier: LGPL-2.1-or-later" !in file.readText().take(512)
                }
            require(missingSpdx.isEmpty()) {
                "Native bridge files without an LGPL SPDX header: ${missingSpdx.sorted()}"
            }
        }
    }

tasks.named("check") {
    dependsOn(
        verifyPublicBoundary,
        ":kmedia-bridge-native-android:check",
        ":kmedia-bridge-native-desktop:check",
    )
}

tasks.register("verifyAll") {
    group = "verification"
    dependsOn("check")
}
