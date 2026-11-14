"""Issue #5066: the CLI worker cap must not change the fiber checksum.

--worker-threads 1 runs the bodies one at a time. --worker-threads 4
runs them together. Both match the single-threaded sum. The fast-path
and checksum assertions themselves live in the suite files.
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

CHECK = ROOT / "tests" / "suite" / "issue_5066_fiber_checksum.aura"


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


class FiberFastPath5066(unittest.TestCase):
    def test_worker_cap_keeps_checksum(self) -> None:
        for workers in ("1", "4"):
            r = _run(["--worker-threads", workers, "--load", str(CHECK)])
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("0 failed", r.stdout)


if __name__ == "__main__":
    unittest.main()
