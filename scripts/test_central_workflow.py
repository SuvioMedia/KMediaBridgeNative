# SPDX-License-Identifier: LGPL-2.1-or-later

import unittest
import re
import subprocess
import tempfile
import textwrap
from pathlib import Path


class CentralWorkflowTest(unittest.TestCase):
    def test_release_identity_binds_the_requested_tag_among_multiple_tags(self) -> None:
        root = Path(__file__).resolve().parents[1]
        workflow = (root / ".github/workflows/publish-maven-central.yml").read_text()
        block = re.search(
            r"      - name: Bind publication to an immutable release on main\n"
            r"        run: \|\n(.*?)(?=      - name:)",
            workflow,
            re.DOTALL,
        )
        self.assertIsNotNone(block)
        script = textwrap.dedent(block.group(1))
        with tempfile.TemporaryDirectory() as directory:
            def git(*arguments: str) -> None:
                subprocess.run(
                    ["git", "-C", directory, *arguments],
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )

            def commit() -> None:
                git(
                    "-c", "user.name=Release test",
                    "-c", "user.email=release-test@example.invalid",
                    "commit", "--allow-empty", "-m", "fixture",
                )

            git("init")
            commit()
            git("tag", "v0.1.0-rc.2")
            git("tag", "v0.1.0-rc.3")
            git("update-ref", "refs/remotes/origin/main", "HEAD")

            def check_identity() -> subprocess.CompletedProcess:
                return subprocess.run(
                    ["bash", "-e", "-c", script],
                    cwd=directory,
                    env={
                        "PATH": "/usr/bin:/bin",
                        "GITHUB_REF": "refs/heads/main",
                        "VERSION": "0.1.0-rc.3",
                    },
                    capture_output=True,
                    text=True,
                )

            self.assertEqual(check_identity().returncode, 0)
            commit()
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            self.assertNotEqual(check_identity().returncode, 0)

    def test_public_release_uses_only_github_hosted_runners(self) -> None:
        root = Path(__file__).resolve().parents[1]
        release = (root / ".github/workflows/release.yml").read_text(encoding="utf-8")
        central = (root / ".github/workflows/publish-maven-central.yml").read_text(
            encoding="utf-8"
        )
        for workflow in (release, central):
            self.assertNotIn("self-hosted", workflow)
            self.assertNotIn("suvio-", workflow)
            self.assertNotIn("repo.suviomedia.cc", workflow)
        self.assertIn("runs-on: ubuntu-24.04", central)
        self.assertIn("SuvioMedia/KMediaBridgeNative", central)
        self.assertIn("github.triggering_actor == 'Shusek'", central)
        self.assertIn("github.triggering_actor == 'Shusek'", release)
        self.assertIn("default: AUTOMATIC", central)

    def test_central_bundle_is_exactly_the_two_public_coordinates(self) -> None:
        root = Path(__file__).resolve().parents[1]
        bundle = (root / "scripts/build_central_bundle.py").read_text(encoding="utf-8")
        self.assertIn('"kmedia-bridge-native-android"', bundle)
        self.assertIn('"kmedia-bridge-native-desktop"', bundle)
        for forbidden in ("kmedia-bridge-api", "kmedia-bridge-ffmpeg", "io/github/shusek"):
            self.assertNotIn(forbidden, bundle)

    def test_existing_deployment_is_polled_for_forty_minutes(self) -> None:
        root = Path(__file__).resolve().parents[1]
        workflow = (root / ".github/workflows/publish-maven-central.yml").read_text(
            encoding="utf-8"
        )

        self.assertIn("timeout-minutes: 45", workflow)
        self.assertIn("for attempt in {1..240}; do", workflow)
        self.assertNotIn("for attempt in {1..90}; do", workflow)
        self.assertIn("instead of", workflow)
        self.assertIn("duplicate upload", workflow)


if __name__ == "__main__":
    unittest.main()
