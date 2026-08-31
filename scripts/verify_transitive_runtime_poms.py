#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path


EXPECTED_RUNTIME_DEPENDENCIES = {
    "kmedia-bridge-native-android": "kmedia-ffmpeg-runtime-android",
    "kmedia-bridge-native-desktop": "kmedia-ffmpeg-runtime-desktop",
}
EXPECTED_PROJECT_URL = "https://github.com/SuvioMedia/KMediaBridgeNative"
EXPECTED_SCM = {
    "connection": "scm:git:https://github.com/SuvioMedia/KMediaBridgeNative.git",
    "developerConnection": "scm:git:ssh://git@github.com/SuvioMedia/KMediaBridgeNative.git",
    "url": EXPECTED_PROJECT_URL,
}


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def dependency_coordinates(pom: Path) -> set[tuple[str, str, str]]:
    coordinates: set[tuple[str, str, str]] = set()
    for dependency in ET.parse(pom).getroot().iter():
        if local_name(dependency.tag) != "dependency":
            continue
        values = {
            local_name(child.tag): (child.text or "").strip()
            for child in dependency
        }
        coordinates.add(
            (values.get("groupId", ""), values.get("artifactId", ""), values.get("version", "")),
        )
    return coordinates


def direct_children(element: ET.Element) -> dict[str, str]:
    return {
        local_name(child.tag): (child.text or "").strip()
        for child in element
    }


def required_project_metadata(pom: Path) -> None:
    project = ET.parse(pom).getroot()
    values = direct_children(project)
    if values.get("url") != EXPECTED_PROJECT_URL:
        raise ValueError(f"published POM has an unexpected project URL: {pom}")
    if not values.get("name") or not values.get("description"):
        raise ValueError(f"published POM is missing its name or description: {pom}")
    licenses = [item for item in project if local_name(item.tag) == "licenses"]
    developers = [item for item in project if local_name(item.tag) == "developers"]
    scm = [item for item in project if local_name(item.tag) == "scm"]
    if len(licenses) != 1 or not list(licenses[0]):
        raise ValueError(f"published POM is missing license metadata: {pom}")
    if len(developers) != 1 or not list(developers[0]):
        raise ValueError(f"published POM is missing developer metadata: {pom}")
    if len(scm) != 1 or direct_children(scm[0]) != EXPECTED_SCM:
        raise ValueError(f"published POM has incomplete SCM metadata: {pom}")


def verify(staging: Path, version: str, runtime_version: str) -> None:
    for artifact, dependency in EXPECTED_RUNTIME_DEPENDENCIES.items():
        pom = staging / "cc/suviomedia" / artifact / version / f"{artifact}-{version}.pom"
        if not pom.is_file():
            raise ValueError(f"published POM is missing: {pom}")
        required_project_metadata(pom)
        coordinates = dependency_coordinates(pom)
        expected = ("cc.suviomedia", dependency, runtime_version)
        if expected not in coordinates:
            raise ValueError(f"{artifact} does not expose the exact shared runtime: {sorted(coordinates)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--staging", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--runtime-version", required=True)
    arguments = parser.parse_args()
    verify(arguments.staging, arguments.version, arguments.runtime_version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
