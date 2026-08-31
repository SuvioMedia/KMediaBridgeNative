#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Create a deterministic source archive for the KMediaBridgeNative release."""

from __future__ import annotations

import argparse
import gzip
import io
import json
import re
import subprocess
import tarfile
from pathlib import Path


SEMVER = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?")
REVISION = re.compile(r"[0-9a-f]{40}")


def git(root: Path, *arguments: str) -> bytes:
    return subprocess.run(["git", "-C", str(root), *arguments], check=True, stdout=subprocess.PIPE).stdout


def add(archive: tarfile.TarFile, name: str, data: bytes, epoch: int, executable: bool) -> None:
    info = tarfile.TarInfo(name)
    info.size, info.mtime, info.uid, info.gid = len(data), epoch, 0, 0
    info.uname = info.gname = "root"
    info.mode = 0o755 if executable else 0o644
    archive.addfile(info, io.BytesIO(data))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--epoch", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    if not SEMVER.fullmatch(arguments.version) or not REVISION.fullmatch(arguments.revision):
        raise ValueError("version and revision must be immutable")
    root = Path(__file__).resolve().parent.parent
    if git(root, "rev-parse", f"{arguments.revision}^{{commit}}").decode().strip() != arguments.revision:
        raise ValueError("revision differs")
    records: list[tuple[str, bytes, bool]] = []
    for raw in git(root, "ls-tree", "-r", "-z", arguments.revision).split(b"\0"):
        if not raw:
            continue
        header, _, raw_path = raw.partition(b"\t")
        mode, kind, object_id = header.split(b" ")
        path = raw_path.decode("utf-8")
        if kind != b"blob" or mode not in {b"100644", b"100755"}:
            raise ValueError(f"unsupported Git entry: {path}")
        if any(part.lower().startswith(".env") for part in Path(path).parts):
            raise ValueError("secret-bearing paths are forbidden")
        records.append((path, git(root, "cat-file", "blob", object_id.decode()), mode == b"100755"))
    manifest = {
        "schemaVersion": 1,
        "project": "KMediaBridgeNative",
        "version": arguments.version,
        "revision": arguments.revision,
        "files": [{"path": path, "size": len(data)} for path, data, _ in records],
    }
    records.append(("SOURCE-MANIFEST.json", (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode(), False))
    if arguments.output.exists():
        raise ValueError("output exists")
    prefix = f"kmedia-bridge-native-{arguments.version}-source"
    with arguments.output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=arguments.epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for name, data, executable in sorted(records):
                    add(archive, f"{prefix}/{name}", data, arguments.epoch, executable)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
