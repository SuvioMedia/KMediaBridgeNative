// SPDX-License-Identifier: LGPL-2.1-or-later

import com.android.build.api.dsl.LibraryExtension
import com.vanniktech.maven.publish.AndroidSingleVariantLibrary
import com.vanniktech.maven.publish.JavadocJar
import com.vanniktech.maven.publish.SourcesJar
import java.nio.file.Files
import java.util.Properties
import org.gradle.api.DefaultTask
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.tasks.InputDirectory
import org.gradle.api.tasks.Optional
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import org.gradle.jvm.tasks.Jar

abstract class VerifyAndroidNativePayload : DefaultTask() {
    @get:Optional
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val payload: DirectoryProperty

    @TaskAction
    fun verify() {
        if (!payload.isPresent) return
        val root = payload.get().asFile
        require(root.isDirectory && !Files.isSymbolicLink(root.toPath())) {
            "Android bridge payload must be a real directory."
        }
        val expectedAbis = setOf("arm64-v8a", "armeabi-v7a")
        val actualAbis = root.resolve("jniLibs").listFiles().orEmpty().filter(File::isDirectory).map(File::getName).toSet()
        require(actualAbis == expectedAbis) { "Android bridge ABI set differs: $actualAbis" }
        expectedAbis.forEach { abi ->
            val files = root.resolve("jniLibs/$abi").listFiles().orEmpty()
            require(files.map(File::getName).toSet() == setOf("libkmediabridge.so") && files.single().length() > 0L) {
                "Android $abi must contain exactly libkmediabridge.so."
            }
        }
        val manifest = root.resolve("android-client.properties")
        require(manifest.isFile) { "Android bridge manifest is missing." }
        val values = Properties().apply { manifest.inputStream().use(::load) }
        require(values.getProperty("sharedRuntimeId")?.isNotBlank() == true) {
            "Android bridge manifest does not bind a shared runtime."
        }
    }
}

plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.vanniktech.maven.publish)
}

val ffmpegRuntimeVersion =
    providers.gradleProperty("kmediaFfmpegRuntimeVersion").orElse("0.1.0-SNAPSHOT").get()
val nativePayload =
    providers.gradleProperty("kmediaBridgeNativeAndroidPayloadDirectory").map(rootProject::file)
val generatedResources = layout.buildDirectory.dir("generated/nativeLegalResources")

extensions.configure<LibraryExtension> {
    namespace = "cc.suviomedia.kmediabridge.runtime.android"
    compileSdk = 37
    defaultConfig { minSdk = 23 }
    sourceSets.getByName("main") {
        nativePayload.orNull?.let { jniLibs.directories.add(it.resolve("jniLibs").absolutePath) }
        resources.directories.add(generatedResources.get().asFile.absolutePath)
    }
}

dependencies {
    api("cc.suviomedia:kmedia-ffmpeg-runtime-android:$ffmpegRuntimeVersion") {
        version { strictly(ffmpegRuntimeVersion) }
    }
}

val prepareLegalResources =
    tasks.register<Sync>("prepareLegalResources") {
        into(generatedResources.map { it.dir("META-INF/kmediabridge-native") })
        from(rootProject.layout.projectDirectory.file("LICENSE"))
        from(rootProject.layout.projectDirectory.file("NOTICE"))
        from(rootProject.layout.projectDirectory.dir("LICENSES")) { into("LICENSES") }
        nativePayload.orNull?.let { payload -> from(payload.resolve("android-client.properties")) }
    }
val verifyNativePayload =
    tasks.register<VerifyAndroidNativePayload>("verifyNativePayload") {
        payload.set(layout.dir(nativePayload))
    }

tasks.named("preBuild") { dependsOn(prepareLegalResources, verifyNativePayload) }
tasks.matching { it.name.startsWith("publish", ignoreCase = true) }.configureEach {
    dependsOn(verifyNativePayload)
    doFirst {
        require(nativePayload.isPresent) {
            "Publishing requires -PkmediaBridgeNativeAndroidPayloadDirectory."
        }
    }
}
tasks.withType<Jar>().matching { it.name.contains("sources", ignoreCase = true) }.configureEach {
    from(rootProject.layout.projectDirectory.dir("native")) { into("native") }
}

publishing.repositories {
    rootProject.providers.gradleProperty("releaseRepository").orNull?.let { repositoryPath ->
        maven { name = "release"; url = uri(repositoryPath) }
    }
}

mavenPublishing {
    configure(AndroidSingleVariantLibrary(JavadocJar.Empty(), SourcesJar.Sources(), "release"))
    coordinates("cc.suviomedia", "kmedia-bridge-native-android", project.version.toString())
    pom {
        name.set("KMediaBridge Native Runtime for Android")
        description.set("LGPL KMediaBridge C/JNI runtime linked to the replaceable KMediaFfmpegRuntime ABI.")
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
        scm { url.set("https://github.com/SuvioMedia/KMediaBridgeNative") }
    }
    publishToMavenCentral()
    if (providers.gradleProperty("signingInMemoryKey").isPresent) signAllPublications()
}
