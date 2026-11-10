"""#5116: a named-let is reentrant on the CLI path, inside one fiber, and under --serve-async.

The same closure runs twice with different captured values. The #5066 fast
path must keep doing that, including when --serve-async-bench drives the file.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

REENTRANT = ROOT / "tests" / "suite" / "issue_5116_named_let_reentrant.aura"
FAST = ROOT / "tests" / "suite" / "issue_5066_fiber_fastpath.aura"


def _run(args: list[str]) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    return subprocess.run(
        [str(AURA_BIN), *args],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )


class NamedLetReentrant5116(unittest.TestCase):
    def test_cli_and_fiber_see_each_capture(self) -> None:
        r = _run(["--load", str(REENTRANT)])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK-5116", r.stdout)

    def test_serve_async_sees_each_capture(self) -> None:
        r = _run(["--serve-async-bench", str(REENTRANT)])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK-5116", r.stdout)

    def test_5066_fastpath_still_passes(self) -> None:
        r = _run(["--load", str(FAST)])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK-5066-fast", r.stdout)


if __name__ == "__main__":
    unittest.main()
