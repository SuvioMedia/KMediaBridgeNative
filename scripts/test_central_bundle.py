# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile


MODULE_PATH = Path(__file__).with_name("build_central_bundle.py")
SPEC = importlib.util.spec_from_file_location("central_bundle", MODULE_PATH)
central = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(central)


class CentralBundleTest(unittest.TestCase):
    def test_normalize_removes_only_gradle_generated_files(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            staging = Path(value)
            version = "0.1.0-rc.1"
            expected: set[Path] = set()
            for artifact in central.ROOT_ARTIFACTS:
                directory = staging / "cc/suviomedia" / artifact / version
                directory.mkdir(parents=True)
                for extension in ("pom", "module"):
                    path = directory / f"{artifact}-{version}.{extension}"
                    path.write_bytes(b"artifact")
                    expected.add(path)
                    for suffix in central.GENERATED_CHECKSUM_SUFFIXES:
                        path.with_name(path.name + suffix).write_bytes(b"generated")
                metadata = directory.parent / "maven-metadata.xml"
                metadata.write_bytes(b"generated")
                for suffix in central.GENERATED_CHECKSUM_SUFFIXES:
                    metadata.with_name(metadata.name + suffix).write_bytes(b"generated")
            central.normalize_staging(staging, version)
            actual = {path for path in staging.rglob("*") if path.is_file()}
            self.assertEqual(expected, actual)

    def test_normalize_rejects_an_unknown_file(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            staging = Path(value)
            version = "0.1.0-rc.1"
            for artifact in central.ROOT_ARTIFACTS:
                directory = staging / "cc/suviomedia" / artifact / version
                directory.mkdir(parents=True)
                (directory / f"{artifact}-{version}.pom").write_bytes(b"artifact")
            (staging / "unexpected").write_bytes(b"unknown")
            with self.assertRaisesRegex(ValueError, "outside the release namespace"):
                central.normalize_staging(staging, version)

    def test_package_contains_only_signed_closed_public_roots(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            staging = root / "staging"
            version = "0.1.0-rc.1"
            expected: set[str] = set()
            for artifact in central.ROOT_ARTIFACTS:
                directory = staging / "cc/suviomedia" / artifact / version
                directory.mkdir(parents=True)
                for extension in ("pom", "module"):
                    path = directory / f"{artifact}-{version}.{extension}"
                    path.write_bytes(b"artifact")
                    signature = path.with_name(path.name + ".asc")
                    signature.write_bytes(b"signature")
                    relative = path.relative_to(staging).as_posix()
                    expected.update(
                        {
                            relative,
                            relative + ".asc",
                            relative + ".md5",
                            relative + ".sha1",
                            relative + ".asc.md5",
                            relative + ".asc.sha1",
                        }
                    )
            bundle = root / "central.zip"
            subprocess.run(
                [
                    sys.executable,
                    str(MODULE_PATH),
                    "--staging",
                    str(staging),
                    "--version",
                    version,
                    "--epoch",
                    "1700000000",
                    "--output",
                    str(bundle),
                ],
                check=True,
            )
            with zipfile.ZipFile(bundle) as archive:
                self.assertEqual(expected, set(archive.namelist()))


if __name__ == "__main__":
    unittest.main()
