// SPDX-License-Identifier: LGPL-2.1-or-later

import com.vanniktech.maven.publish.JavaLibrary
import com.vanniktech.maven.publish.JavadocJar
import com.vanniktech.maven.publish.SourcesJar
import java.security.MessageDigest
import java.util.Properties
import org.gradle.api.DefaultTask
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.Optional
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.gradle.jvm.tasks.Jar

abstract class VerifyDesktopNativePayload : DefaultTask() {
    @get:Optional
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val payload: DirectoryProperty

    @TaskAction
    fun verify() {
        if (!payload.isPresent) return
        val nativeRoot = payload.get().asFile.resolve("META-INF/kmediabridge/native")
        val expectedPlatforms = setOf("linux-x86_64", "linux-aarch64", "macos-aarch64", "windows-x86_64")
        val actualPlatforms = nativeRoot.listFiles().orEmpty().filter(File::isDirectory).map(File::getName).toSet()
        require(actualPlatforms == expectedPlatforms) { "Desktop bridge platform matrix differs: $actualPlatforms" }
        val runtimeIds = mutableSetOf<String>()
        expectedPlatforms.forEach { platform ->
            val directory = nativeRoot.resolve(platform)
            val manifestFile = directory.resolve("manifest.properties")
            require(manifestFile.isFile) { "Missing bridge manifest for $platform." }
            val manifest = Properties().apply { manifestFile.inputStream().use(::load) }
            val libraryName = manifest.getProperty("library.0.name").orEmpty()
            val library = directory.resolve(libraryName)
            require(manifest.getProperty("platform") == platform && manifest.getProperty("library.count") == "1") {
                "Bridge manifest differs for $platform."
            }
            require(directory.listFiles().orEmpty().map(File::getName).toSet() == setOf("manifest.properties", libraryName)) {
                "Bridge payload inventory is not closed for $platform."
            }
            val sha256 =
                MessageDigest.getInstance("SHA-256").digest(library.readBytes()).joinToString("") { byte ->
                    "%02x".format(byte.toInt() and 0xff)
                }
            require(sha256 == manifest.getProperty("library.0.sha256")) {
                "Bridge payload checksum differs for $platform."
            }
            runtimeIds += manifest.getProperty("sharedRuntimeId").orEmpty()
        }
        require(runtimeIds.size == 1 && runtimeIds.single().isNotBlank()) {
            "Desktop bridge payloads must bind one shared runtime ID."
        }
    }
}

plugins {
    `java-library`
    alias(libs.plugins.vanniktech.maven.publish)
}

val ffmpegRuntimeVersion =
    providers.gradleProperty("kmediaFfmpegRuntimeVersion").orElse("0.1.0-SNAPSHOT").get()
val nativePayload =
    providers.gradleProperty("kmediaBridgeNativeDesktopPayloadDirectory").map(rootProject::file)

java {
    toolchain.languageVersion.set(JavaLanguageVersion.of(17))
    withSourcesJar()
}

dependencies {
    api("cc.suviomedia:kmedia-ffmpeg-runtime-desktop:$ffmpegRuntimeVersion") {
        version { strictly(ffmpegRuntimeVersion) }
    }
}

sourceSets.main {
    nativePayload.orNull?.let { resources.srcDir(it) }
}

tasks.withType<Jar>().configureEach {
    isPreserveFileTimestamps = false
    isReproducibleFileOrder = true
}
tasks.named<ProcessResources>("processResources") {
    from(rootProject.layout.projectDirectory.file("LICENSE")) { into("META-INF") }
    from(rootProject.layout.projectDirectory.file("NOTICE")) { into("META-INF") }
    from(rootProject.layout.projectDirectory.dir("LICENSES")) { into("META-INF/LICENSES") }
}
tasks.named<Jar>("sourcesJar") {
    from(rootProject.layout.projectDirectory.dir("native")) { into("native") }
}

val verifyNativePayload =
    tasks.register<VerifyDesktopNativePayload>("verifyNativePayload") {
        payload.set(layout.dir(nativePayload))
    }
tasks.named("check") { dependsOn(verifyNativePayload) }
tasks.matching { it.name.startsWith("publish", ignoreCase = true) }.configureEach {
    dependsOn(verifyNativePayload)
    doFirst {
        require(nativePayload.isPresent) {
            "Publishing requires -PkmediaBridgeNativeDesktopPayloadDirectory."
        }
    }
}

publishing.repositories {
    rootProject.providers.gradleProperty("releaseRepository").orNull?.let { repositoryPath ->
        maven { name = "release"; url = uri(repositoryPath) }
    }
}

mavenPublishing {
    configure(JavaLibrary(JavadocJar.Empty(), SourcesJar.Sources()))
    coordinates("cc.suviomedia", "kmedia-bridge-native-desktop", project.version.toString())
    pom {
        name.set("KMediaBridge Native Runtime for Desktop")
        description.set("LGPL KMediaBridge C runtime linked to the replaceable KMediaFfmpegRuntime ABI.")
        inceptionYear.set("2026")
        url.set("https://github.com/SuvioMedia/KMediaBridgeNative")
        licenses {
            license {
                name.set("GNU Lesser General Public License, version 2.1 or later")
                url.set("https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html")
                distribution.set("repo")
            }
        }
        developers { developer { id.set("SuvioMedia"); name.set("SuvioMedia") } }
        scm {
            connection.set("scm:git:https://github.com/SuvioMedia/KMediaBridgeNative.git")
            developerConnection.set("scm:git:ssh://git@github.com/SuvioMedia/KMediaBridgeNative.git")
            url.set("https://github.com/SuvioMedia/KMediaBridgeNative")
        }
    }
    publishToMavenCentral()
    if (providers.gradleProperty("signingInMemoryKey").isPresent) signAllPublications()
}
