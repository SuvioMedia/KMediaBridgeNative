#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
from pathlib import Path


EXPECTED = {
    "android-arm64-v8a": ("libkmediabridge.so", "AArch64"),
    "android-armeabi-v7a": ("libkmediabridge.so", "ARM"),
    "linux-x86_64": ("libkmediabridge.so", "Advanced Micro Devices X86-64"),
    "linux-aarch64": ("libkmediabridge.so", "AArch64"),
    "macos-aarch64": ("libkmediabridge.dylib", "arm64"),
    "windows-x86_64": ("kmediabridge.dll", "pei-x86-64"),
}
RUNTIME_ID = re.compile(r"kmediaffmpeg-9\.0\.1-ass-0\.17\.5-[0-9a-f]{16}")
SEMVER = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?")
SHA256 = re.compile(r"[0-9a-f]{64}")
FORBIDDEN = re.compile(r"(?:libav.+-kmb|libkmediampv_av|libkmediabridge_av)")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def props(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key or not value or key in result:
            raise ValueError("client manifest is malformed")
        result[key] = value
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target", choices=EXPECTED, required=True)
    parser.add_argument("--readelf", default="readelf")
    args = parser.parse_args()
    expected_name, expected_arch = EXPECTED[args.target]
    runtime = args.output / "runtime"
    files = {path.name for path in runtime.iterdir() if path.is_file()}
    if files != {expected_name}:
        raise ValueError(f"KMediaBridge client inventory differs: {files}")
    manifest = props(args.output / "manifest.properties")
    library = runtime / expected_name
    if manifest.get("platform") != args.target or manifest.get("library.count") != "1":
        raise ValueError("KMediaBridge client manifest target or inventory differs")
    if manifest.get("library.0.name") != expected_name or manifest.get("library.0.sha256") != sha256(library):
        raise ValueError("KMediaBridge client hash differs from its manifest")
    if manifest.get("library.0.role") != "BRIDGE" or not RUNTIME_ID.fullmatch(manifest.get("sharedRuntimeId", "")):
        raise ValueError("KMediaBridge client does not bind one shared runtime ID")
    runtime_version = manifest.get("sharedRuntimeVersion", "")
    if not SEMVER.fullmatch(runtime_version):
        raise ValueError("KMediaBridge client omits the exact shared runtime distribution version")
    expected_source = (
        f"https://github.com/SuvioMedia/KMediaFfmpegRuntime/releases/download/v{runtime_version}/"
        f"kmedia-ffmpeg-runtime-{runtime_version}-corresponding-source.tar.gz"
    )
    if manifest.get("sourceOfferUrl") != expected_source or not SHA256.fullmatch(manifest.get("sourceSha256", "")):
        raise ValueError("KMediaBridge client has incomplete shared-runtime source evidence")
    shared_runtime = props(args.output / "compliance/kmediaffmpeg-runtime.properties")

    def runtime_feature(name: str, legacy_default: bool) -> bool:
        value = shared_runtime.get(f"feature.{name}")
        if value is None:
            return legacy_default
        if value not in {"true", "false"}:
            raise ValueError(f"shared runtime has an invalid feature.{name} value")
        return value == "true"

    full_desktop = args.target in {"macos-aarch64", "windows-x86_64"}
    tone_map = (
        args.target.startswith("android-") or full_desktop
    ) and runtime_feature(
        "hdrToSdrToneMap",
        args.target.startswith("android-") or args.target == "macos-aarch64",
    )
    subtitle_burn = full_desktop and runtime_feature(
        "subtitleBurnIn",
        args.target == "macos-aarch64",
    )
    avfoundation_compatibility = full_desktop and runtime_feature(
        "avcAacTranscode",
        args.target == "macos-aarch64",
    )
    if manifest.get("capability.canToneMapToSdr") != str(tone_map).lower():
        raise ValueError("tone-map capability differs from the compiled target policy")
    if manifest.get("capability.canTranscodeVideo") != str(tone_map).lower():
        raise ValueError("video-transcode capability differs from the compiled target policy")
    if manifest.get("capability.canTranscodeAudio") != str(avfoundation_compatibility).lower():
        raise ValueError("audio-transcode capability differs from the compiled target policy")
    if manifest.get("capability.canBurnSubtitles") != str(subtitle_burn).lower():
        raise ValueError("subtitle-burn capability differs from the compiled target policy")
    input_containers = set(manifest.get("capability.inputContainers", "").split(","))
    if ({"AVI", "ASF"}.issubset(input_containers)) != avfoundation_compatibility:
        raise ValueError("legacy input capabilities differ from the compiled target policy")
    if manifest.get("runtimeFlavor") != ("SUBTITLE_BURN_IN_SDR" if subtitle_burn else "REMUX_ONLY"):
        raise ValueError("runtime flavor differs from the compiled target policy")
    if args.target.startswith(("macos-",)):
        architecture = subprocess.run(["lipo", "-archs", library], check=True, text=True, stdout=subprocess.PIPE).stdout
        dependencies = subprocess.run(["otool", "-L", library], check=True, text=True, stdout=subprocess.PIPE).stdout
    elif args.target.startswith("windows-"):
        architecture = subprocess.run(["objdump", "-f", library], check=True, text=True, stdout=subprocess.PIPE).stdout
        dependencies = subprocess.run(["objdump", "-p", library], check=True, text=True, stdout=subprocess.PIPE).stdout
    else:
        architecture = subprocess.run([args.readelf, "-h", library], check=True, text=True, stdout=subprocess.PIPE).stdout
        dependencies = subprocess.run([args.readelf, "-d", library], check=True, text=True, stdout=subprocess.PIPE).stdout
    if expected_arch not in architecture:
        raise ValueError("KMediaBridge client architecture differs from policy")
    if FORBIDDEN.search(dependencies) or "kmediaffmpeg_avutil" not in dependencies:
        raise ValueError("KMediaBridge client is not linked only to the shared runtime ABI")
    if ("kmediaffmpeg_avfilter" in dependencies) != (subtitle_burn or avfoundation_compatibility):
        raise ValueError("KMediaBridge subtitle-filter linkage differs from target policy")
    if ("kmediaffmpeg_swresample" in dependencies) != avfoundation_compatibility:
        raise ValueError("KMediaBridge audio-conversion linkage differs from target policy")
    if any(path.suffix in {".a", ".o", ".obj"} for path in args.output.rglob("*")):
        raise ValueError("KMediaBridge client output contains a static object")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
