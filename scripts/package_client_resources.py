#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

import argparse
import shutil
from pathlib import Path


PLATFORMS = {"linux-x86_64", "linux-aarch64", "macos-aarch64", "windows-x86_64"}


def props(path: Path) -> dict[str, str]:
    return dict(line.split("=", 1) for line in path.read_text().splitlines() if line and not line.startswith("#"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    clients: dict[str, Path] = {}
    for value in args.client:
        platform, separator, directory = value.partition("=")
        if not separator or platform not in PLATFORMS or platform in clients:
            raise ValueError(f"invalid client mapping: {value}")
        clients[platform] = Path(directory).resolve()
    if set(clients) != PLATFORMS:
        raise ValueError("desktop client platform set differs")
    ids: set[str] = set()
    output = args.output.resolve()
    if output.exists():
        raise ValueError("output already exists")
    for platform, source in sorted(clients.items()):
        manifest = props(source / "manifest.properties")
        if manifest.get("platform") != platform or manifest.get("library.count") != "1":
            raise ValueError("client manifest platform or inventory differs")
        ids.add(manifest["sharedRuntimeId"])
        destination = output / "META-INF/kmediabridge/native" / platform
        destination.mkdir(parents=True)
        shutil.copyfile(source / "manifest.properties", destination / "manifest.properties")
        library = source / "runtime" / manifest["library.0.name"]
        shutil.copyfile(library, destination / library.name)
    if len(ids) != 1:
        raise ValueError("desktop clients target more than one shared runtime ID")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
