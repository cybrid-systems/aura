"""Issue #5116: a named-let stays reentrant under --serve-async-bench.

The host and one-fiber faces live in the suite file. This process test
is the scheduler face, which the suite runner does not use for that file.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

REENTRANT = ROOT / "tests" / "suite" / "issue_5116_named_let_reentrant.aura"


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
    def test_serve_async_sees_each_capture(self) -> None:
        r = _run(["--serve-async-bench", str(REENTRANT)])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("0 failed", r.stdout)


if __name__ == "__main__":
    unittest.main()
