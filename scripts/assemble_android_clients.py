#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

import argparse
import hashlib
import shutil
from pathlib import Path


def props(path: Path) -> dict[str, str]:
    return dict(line.split("=", 1) for line in path.read_text().splitlines() if line and not line.startswith("#"))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arm64", type=Path, required=True)
    parser.add_argument("--armv7", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    inputs = [("arm64-v8a", args.arm64.resolve()), ("armeabi-v7a", args.armv7.resolve())]
    manifests = [(abi, source, props(source / "manifest.properties")) for abi, source in inputs]
    runtime_ids = {manifest["sharedRuntimeId"] for _, _, manifest in manifests}
    revisions = {manifest["buildRecipeRevision"] for _, _, manifest in manifests}
    if len(runtime_ids) != 1 or len(revisions) != 1:
        raise ValueError("Android clients do not target one runtime ID and recipe revision")
    output = args.output.resolve()
    if output.exists():
        raise ValueError("output already exists")
    values = [
        ("schemaVersion", "1"), ("available", "true"), ("abiVersion", "5"),
        ("sharedRuntimeId", next(iter(runtime_ids))), ("ffmpegVersion", "9.0.1"),
        ("ffmpegLicenseSpdx", "LGPL-2.1-or-later"),
        ("ffmpegSourceArchiveUrl", manifests[0][2]["sourceOfferUrl"]),
        ("ffmpegSourceArchiveSha256", manifests[0][2]["sourceSha256"]),
        ("buildRecipeUrl", manifests[0][2]["buildRecipeUrl"]),
        ("buildRecipeRevision", next(iter(revisions))),
        ("exactCorrespondingSourceAvailable", "true"), ("dynamicLinkingVerified", "true"),
        ("feature.hdrToSdrToneMap", "true"), ("feature.subtitleBurnIn", "false"),
        ("abi.count", "2"),
    ]
    for index, (abi, source, _) in enumerate(manifests):
        library = source / "runtime/libkmediabridge.so"
        destination = output / "jniLibs" / abi / library.name
        destination.parent.mkdir(parents=True)
        shutil.copyfile(library, destination)
        values.extend([
            (f"abi.{index}.name", abi),
            (f"abi.{index}.libkmediabridge.so.sha256", sha256(destination)),
        ])
    (output / "android-client.properties").write_text(
        "\n".join(f"{key}={value}" for key, value in values) + "\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
