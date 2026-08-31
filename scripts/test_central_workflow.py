# SPDX-License-Identifier: LGPL-2.1-or-later

import unittest
from pathlib import Path


class CentralWorkflowTest(unittest.TestCase):
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
        self.assertIn("default: USER_MANAGED", central)

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
