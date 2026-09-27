#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Build the public KMediaBridge native runtime against an immutable FFmpeg SDK."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TARGETS = (
    "android-arm64-v8a", "android-armeabi-v7a", "linux-x86_64", "linux-aarch64",
    "macos-aarch64", "windows-x86_64",
)
ANDROID = {
    "android-arm64-v8a": ("arm64-v8a", "aarch64-linux-android", "AArch64"),
    "android-armeabi-v7a": ("armeabi-v7a", "armv7a-linux-androideabi", "ARM"),
}
RUNTIME_ID = re.compile(r"kmediaffmpeg-9\.0\.1-ass-0\.17\.5-[0-9a-f]{16}")
FULL_DESKTOP_TARGETS = {"macos-aarch64", "windows-x86_64"}


def run(*command: str) -> str:
    print("+", " ".join(command), flush=True)
    return subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE).stdout


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def properties(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="ISO-8859-1").splitlines():
        if not line or line.startswith(("#", "!")):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key or not value or key in result:
            raise ValueError(f"malformed property in {path}")
        result[key] = value
    return result


def validate_target(target: str) -> None:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if target == "macos-aarch64" and not (system == "darwin" and machine in {"arm64", "aarch64"}):
        raise ValueError("macOS client requires Apple Silicon")
    if target == "linux-x86_64" and not (system == "linux" and machine in {"x86_64", "amd64"}):
        raise ValueError("Linux x86_64 client requires a matching host")
    if target == "linux-aarch64" and not (system == "linux" and machine in {"arm64", "aarch64"}):
        raise ValueError("Linux ARM64 client requires a matching host")
    if target == "windows-x86_64" and not (
        system.startswith(("windows", "mingw", "msys")) and machine in {"x86_64", "amd64"}
    ):
        raise ValueError("Windows x86_64 client requires a matching MSYS2 UCRT64 host")


def runtime_contract(sdk: Path, target: str, expected_version: str) -> dict[str, str]:
    manifest = properties(sdk / "runtime.properties")
    if not RUNTIME_ID.fullmatch(manifest.get("runtimeId", "")):
        raise ValueError("runtime SDK has an invalid runtime ID")
    if manifest.get("version.ffmpeg") != "9.0.1" or manifest.get("version.libass") != "0.17.5":
        raise ValueError("runtime SDK component versions differ from the client contract")
    if manifest.get("distributionVersion") != expected_version:
        raise ValueError("runtime SDK distribution version differs from the client contract")
    expected_platform = "android" if target.startswith("android-") else target.split("-", 1)[0]
    expected_abi = ANDROID[target][0] if target in ANDROID else "aarch64" if target.endswith("aarch64") else "x86_64"
    if manifest.get("platform") != expected_platform or manifest.get("abi") != expected_abi:
        raise ValueError("runtime SDK target differs from the requested client")
    if not (sdk / "include").is_dir() or not (sdk / "lib").is_dir():
        raise ValueError("runtime SDK omits headers or libraries")
    return manifest


def target_features(target: str, runtime: dict[str, str]) -> dict[str, bool]:
    legacy_defaults = {
        "hdrToSdrToneMap": target in ANDROID or target == "macos-aarch64",
        "subtitleBurnIn": target == "macos-aarch64",
        "avcAacTranscode": target == "macos-aarch64",
    }

    def enabled(name: str) -> bool:
        value = runtime.get(f"feature.{name}")
        if value is None:
            return legacy_defaults[name]
        if value not in {"true", "false"}:
            raise ValueError(f"runtime SDK has an invalid feature.{name} value")
        return value == "true"

    tone_map_target = target in ANDROID or target in FULL_DESKTOP_TARGETS
    full_desktop_target = target in FULL_DESKTOP_TARGETS
    return {
        "toneMap": tone_map_target and enabled("hdrToSdrToneMap"),
        "subtitleBurn": full_desktop_target and enabled("subtitleBurnIn"),
        "avcAacTranscode": full_desktop_target and enabled("avcAacTranscode"),
    }


def android_tools(ndk: Path, target: str) -> dict[str, str]:
    abi, triple, _ = ANDROID[target]
    host = "darwin-x86_64" if platform.system() == "Darwin" else "linux-x86_64"
    tools = ndk / "toolchains/llvm/prebuilt" / host / "bin"
    values = {
        "abi": abi,
        "cc": str(tools / f"{triple}23-clang"),
        "strip": str(tools / "llvm-strip"),
        "readelf": str(tools / "llvm-readelf"),
    }
    for name in ("cc", "strip", "readelf"):
        if not Path(values[name]).is_file():
            raise ValueError(f"Android NDK tool is missing: {values[name]}")
    return values


def compile_client(
    target: str,
    sdk: Path,
    output: Path,
    ndk: Path | None,
    feature_policy: dict[str, bool],
) -> tuple[Path, str | None]:
    runtime = output / "runtime"
    runtime.mkdir()
    sources = [
        ROOT / "native/src/kmedia_bridge.c",
        ROOT / "native/src/kmedia_bridge_hevc.c",
        ROOT / "native/src/kmedia_bridge_subtitles.c",
        ROOT / "native/src/kmedia_bridge_hdr_math.c",
        ROOT / "native/src/kmedia_bridge_tonemap.c",
        ROOT / "native/src/kmedia_bridge_avfoundation.c",
    ]
    includes = ["-I", str(ROOT / "native/include"), "-I", str(ROOT / "native/src"), "-I", str(sdk / "include")]
    compile_definitions: list[str] = []
    feature_libraries: list[str] = []
    if feature_policy["toneMap"]:
        compile_definitions.append("-DKMB_ENABLE_HDR_TO_SDR=1")
    if feature_policy["subtitleBurn"]:
        compile_definitions.append("-DKMB_ENABLE_SUBTITLE_BURN_IN=1")
    if feature_policy["avcAacTranscode"]:
        compile_definitions.append("-DKMB_ENABLE_AVFOUNDATION_TRANSCODE=1")
    if feature_policy["subtitleBurn"] or feature_policy["avcAacTranscode"]:
        feature_libraries.append("-lkmediaffmpeg_avfilter")
    if feature_policy["avcAacTranscode"]:
        feature_libraries.append("-lkmediaffmpeg_swresample")
    common = [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fvisibility=hidden",
        *compile_definitions, *includes, *(str(source) for source in sources),
        "-L", str(sdk / "lib"), "-lkmediaffmpeg_avformat", "-lkmediaffmpeg_avcodec",
        "-lkmediaffmpeg_swscale", "-lkmediaffmpeg_avutil", *feature_libraries, "-lm",
    ]
    readelf: str | None = None
    if target in ANDROID:
        if ndk is None:
            raise ValueError("--ndk is required for Android")
        tools = android_tools(ndk, target)
        output_file = runtime / "libkmediabridge.so"
        jni = ndk / "toolchains/llvm/prebuilt" / ("darwin-x86_64" if platform.system() == "Darwin" else "linux-x86_64") / "sysroot/usr/include"
        run(
            tools["cc"], "-shared", "-fPIC", *common,
            "-I", str(jni), str(ROOT / "native/android/kmedia_bridge_android_jni.c"),
            "-landroid", "-lmediandk", "-llog", "-ldl",
            "-Wl,-soname,libkmediabridge.so", "-Wl,-z,relro", "-Wl,-z,now",
            "-Wl,-z,max-page-size=16384", "-o", str(output_file),
        )
        run(tools["strip"], "--strip-unneeded", str(output_file))
        readelf = tools["readelf"]
    elif target == "macos-aarch64":
        output_file = runtime / "libkmediabridge.dylib"
        run(
            "cc", "-dynamiclib", *common, "-Wl,-install_name,@rpath/libkmediabridge.dylib",
            "-Wl,-rpath,@loader_path", "-o", str(output_file),
        )
    elif target.startswith("linux-"):
        output_file = runtime / "libkmediabridge.so"
        run(
            "cc", "-shared", "-fPIC", *common, "-Wl,-soname,libkmediabridge.so",
            "-Wl,-rpath,$ORIGIN", "-o", str(output_file),
        )
    else:
        output_file = runtime / "kmediabridge.dll"
        run(
            "cc", "-shared", *common, "-static-libgcc", "-Wl,--major-os-version,10",
            "-Wl,--minor-os-version,0", "-o", str(output_file),
        )
    return output_file, readelf


def write_manifest(
    output: Path,
    target: str,
    runtime: dict[str, str],
    library: Path,
    revision: str,
    runtime_version: str,
    runtime_source_sha256: str,
    features: dict[str, bool],
) -> None:
    tone_map = features["toneMap"]
    subtitle_burn = features["subtitleBurn"]
    avfoundation_compatibility = features["avcAacTranscode"]
    input_containers = ["MATROSKA", "WEBM", "MP4", "FRAGMENTED_MP4", "MPEG_TS"]
    if avfoundation_compatibility:
        input_containers.extend(["AVI", "ASF"])
    values = [
        ("schemaVersion", "1"), ("platform", target), ("abiVersion", "5"),
        ("sharedRuntimeId", runtime["runtimeId"]),
        ("sharedRuntimeVersion", runtime["distributionVersion"]),
        ("ffmpegVersion", runtime["version.ffmpeg"]),
        ("ffmpegLicenseSpdx", runtime["license.ffmpeg"]),
        ("ffmpegReportedLicense", "LGPL version 2.1 or later"),
        ("sourceOfferUrl", f"https://github.com/SuvioMedia/KMediaFfmpegRuntime/releases/download/v{runtime_version}/kmedia-ffmpeg-runtime-{runtime_version}-corresponding-source.tar.gz"),
        ("sourceSha256", runtime_source_sha256),
        ("buildRecipeUrl", f"https://github.com/SuvioMedia/KMediaBridgeNative/tree/{revision}/native"),
        ("buildRecipeRevision", revision), ("exactCorrespondingSourceAvailable", "true"),
        ("dynamicLinkingVerified", "true"),
        ("runtimeFlavor", "SUBTITLE_BURN_IN_SDR" if subtitle_burn else "REMUX_ONLY"),
        ("capability.inputContainers", ",".join(input_containers)),
        ("capability.outputs", "CMAF_FRAGMENT_STREAM"), ("capability.canProbe", "true"),
        ("capability.canCopyVideo", "true"), ("capability.canToneMapToSdr", str(tone_map).lower()),
        ("capability.canConvertDolbyVisionProfile7", "false"),
        ("capability.supportsLiveInput", "false"), ("capability.supportsEncryptedInput", "false"),
        ("capability.supportsRemoteInput", "false"),
        ("capability.canTranscodeVideo", str(tone_map).lower()),
        ("capability.canTranscodeAudio", str(avfoundation_compatibility).lower()),
        ("capability.canBurnSubtitles", str(subtitle_burn).lower()),
        ("component.count", "0"), ("library.count", "1"),
        ("library.0.name", library.name), ("library.0.sha256", sha256(library)),
        ("library.0.role", "BRIDGE"),
    ]
    manifest = "\n".join(f"{key}={value}" for key, value in values) + "\n"
    (output / "manifest.properties").write_bytes(manifest.encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=TARGETS, required=True)
    parser.add_argument("--runtime-sdk", type=Path, required=True)
    parser.add_argument("--runtime-version", required=True)
    parser.add_argument("--runtime-source-sha256", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?", arguments.version):
        raise ValueError("version must be immutable SemVer")
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?", arguments.runtime_version):
        raise ValueError("runtime version must be immutable SemVer")
    if not re.fullmatch(r"[0-9a-f]{64}", arguments.runtime_source_sha256):
        raise ValueError("runtime corresponding-source SHA-256 must be lowercase hex")
    if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", arguments.revision):
        raise ValueError("revision must be a full lowercase Git object ID")
    validate_target(arguments.target)
    sdk = arguments.runtime_sdk.resolve()
    output = arguments.output.resolve()
    if output.exists():
        raise ValueError("output already exists")
    output.mkdir(parents=True)
    runtime = runtime_contract(sdk, arguments.target, arguments.runtime_version)
    features = target_features(arguments.target, runtime)
    library, readelf = compile_client(
        arguments.target,
        sdk,
        output,
        arguments.ndk.resolve() if arguments.ndk else None,
        features,
    )
    write_manifest(
        output,
        arguments.target,
        runtime,
        library,
        arguments.revision,
        arguments.runtime_version,
        arguments.runtime_source_sha256,
        features,
    )
    client_sdk = output / "sdk" / arguments.target
    (client_sdk / "include").mkdir(parents=True)
    (client_sdk / "lib").mkdir()
    shutil.copyfile(ROOT / "native/include/kmedia_bridge.h", client_sdk / "include/kmedia_bridge.h")
    shutil.copyfile(library, client_sdk / "lib" / library.name)
    shutil.copyfile(output / "manifest.properties", client_sdk / "manifest.properties")
    compliance = output / "compliance"
    compliance.mkdir()
    shutil.copyfile(sdk / "runtime.properties", compliance / "kmediaffmpeg-runtime.properties")
    (compliance / "client-source-sha256.json").write_text(json.dumps({
        path.relative_to(ROOT).as_posix(): sha256(path)
        for path in sorted((ROOT / "native").rglob("*")) if path.is_file()
    }, indent=2, sort_keys=True) + "\n")
    verification = [sys.executable, "-B", ROOT / "scripts/verify_client_output.py", "--output", output, "--target", arguments.target]
    if readelf is not None:
        verification.extend(["--readelf", readelf])
    run(*(str(value) for value in verification))
    print(runtime["runtimeId"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
