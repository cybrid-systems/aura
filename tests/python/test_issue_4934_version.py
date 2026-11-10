#!/usr/bin/env python3
"""Regression for #4934: `aura --version` / `-V` print `aura <version> (<git sha>)`.

Before: `aura --version` was "error: unknown option '--version'" (rc=1), so
install.sh and the dist smoke tests could not report what got installed.
"""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

LINE = re.compile(
    r"^aura (dev|nightly|\d+\.\d+\.\d+\S*) \(([0-9a-f]{9}|[0-9a-f]{40}|unknown)\)$"
)


class Version(unittest.TestCase):
    def test_both_spellings(self) -> None:
        outs = []
        for flag in ("--version", "-V"):
            r = subprocess.run([str(AURA_BIN), flag], capture_output=True, text=True, timeout=30)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stderr, "")
            self.assertRegex(r.stdout.strip(), LINE)
            outs.append(r.stdout)
        self.assertEqual(outs[0], outs[1])


if __name__ == "__main__":
    unittest.main()
