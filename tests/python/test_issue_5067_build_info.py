"""#5067: BUILD-INFO commit resolution must not silently become "unknown".

Does not build a release tarball or touch tags, Releases, or images.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RESOLVE = ROOT / "scripts" / "tools" / "resolve_git_sha.sh"


class ResolveGitSha(unittest.TestCase):
    def test_env_wins_over_git(self) -> None:
        full = "cf7353373ea276bc03e8364592814382ebd62539"
        r = subprocess.run(
            [str(RESOLVE), str(ROOT)],
            capture_output=True,
            text=True,
            check=False,
            env={**os.environ, "AURA_GIT_SHA": full},
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), full)

    def test_checkout_sha_is_full(self) -> None:
        r = subprocess.run(
            [str(RESOLVE), str(ROOT)],
            capture_output=True,
            text=True,
            check=False,
            env={k: v for k, v in os.environ.items() if k != "AURA_GIT_SHA"},
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout.strip(), r"^[0-9a-f]{40}$")

    def test_required_sha_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run(
                [str(RESOLVE), tmp],
                capture_output=True,
                text=True,
                check=False,
                env={
                    k: v
                    for k, v in os.environ.items()
                    if k not in ("AURA_GIT_SHA", "GIT_DIR")
                }
                | {"AURA_REQUIRE_GIT_SHA": "1"},
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertNotIn("unknown", r.stdout)

    def test_scripts_pass_sha_and_image_label(self) -> None:
        bundle = (ROOT / "scripts" / "tools" / "bundle.sh").read_text()
        docker = (ROOT / "scripts" / "docker" / "Dockerfile").read_text()
        dist = (ROOT / ".github" / "workflows" / "dist.yml").read_text()
        release = (ROOT / ".github" / "workflows" / "release.yml").read_text()
        self.assertIn("resolve_git_sha.sh", bundle)
        self.assertIn("org.opencontainers.image.version", docker)
        self.assertIn("AURA_VERSION", docker)
        self.assertIn("AURA_BUILD_GIT_SHA", dist)
        self.assertIn("AURA_REVISION", dist)
        self.assertIn("AURA_GIT_SHA", release)
        self.assertIn("safe.directory", release)


if __name__ == "__main__":
    unittest.main()
