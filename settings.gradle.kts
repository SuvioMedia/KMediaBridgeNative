// SPDX-License-Identifier: LGPL-2.1-or-later

pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        gradlePluginPortal()
        mavenCentral()
    }
}

plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "1.0.0"
}

rootProject.name = "KMediaBridgeNative"

providers.gradleProperty("kmediaFfmpegRuntimeProjectDir").orNull?.let { runtimeDirectory ->
    includeBuild(runtimeDirectory) {
        dependencySubstitution {
            substitute(module("cc.suviomedia:kmedia-ffmpeg-runtime-android"))
                .using(project(":kmedia-ffmpeg-runtime-android"))
            substitute(module("cc.suviomedia:kmedia-ffmpeg-runtime-desktop"))
                .using(project(":kmedia-ffmpeg-runtime-desktop"))
        }
    }
}

dependencyResolutionManagement {
    repositories {
        providers.gradleProperty("kmediaFfmpegRuntimeRepository").orNull?.let { repositoryPath ->
            maven { url = uri(repositoryPath) }
        }
        google()
        mavenCentral()
    }
}

include(":kmedia-bridge-native-android")
project(":kmedia-bridge-native-android").projectDir = file("ffmpeg-runtime-android")
include(":kmedia-bridge-native-desktop")
project(":kmedia-bridge-native-desktop").projectDir = file("ffmpeg-runtime-desktop")
