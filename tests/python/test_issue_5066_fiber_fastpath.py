"""#5066: integer fast path stays exact with the CLI fiber worker cap.

The cap (--worker-threads N) must not change results: one worker runs the
bodies one at a time, four workers run them together, both match the
single-threaded sum. A local shadow of `+` stays off the primitive.
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

FAST = ROOT / "tests" / "suite" / "issue_5066_fiber_fastpath.aura"
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
    def test_fastpath_checksum_shadow_and_set(self) -> None:
        r = _run(["--load", str(FAST)])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK-5066-fast", r.stdout)

    def test_worker_cap_keeps_checksum(self) -> None:
        for workers in ("1", "4"):
            r = _run(["--worker-threads", workers, "--load", str(CHECK)])
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("OK-5066", r.stdout)


if __name__ == "__main__":
    unittest.main()
