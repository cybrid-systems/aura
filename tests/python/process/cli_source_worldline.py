#!/usr/bin/env python3
r"""Regression for #5026: the terminal shows the source worldline.

Before: \`aura new evolve\` stepped \`(quote (0 0))\` 40 times and printed a
three-line summary pointing at out/report.html. The first run never showed a
definition being written, rejected, and restored.

Now: one live definition, four fixed candidates, and the live source after each
decision. \`aura demo\` runs the same template source without creating a project.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def run(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        env.pop(k, None)
    return subprocess.run(
        [str(AURA_BIN), *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
        stdin=subprocess.DEVNULL,
    )


EXPECTED = (
    "1 ROLLBACK  (define (guess) 10)   live (define (guess) 0)\n"
    "2 KEEP      (define (guess) 7)    live (define (guess) 7)\n"
    "3 ROLLBACK  (define (guess) 2)    live (define (guess) 7)\n"
    "4 KEEP      (define (guess) 42)   live (define (guess) 42)\n"
    "(guess) => 42\n"
)


class Worldline(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_demo(self) -> None:
        r = run(["demo"], self.d)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout, EXPECTED)
        self.assertEqual(list(self.d.iterdir()), [], "demo must not write into cwd")

    def test_new_evolve_matches_demo(self) -> None:
        r = run(["new", "evolve", "demo"], self.d)
        self.assertEqual(r.returncode, 0, r.stderr)
        p = self.d / "demo"
        self.assertFalse((p / "report.aura").exists())
        ran = run(["run"], p)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout, EXPECTED)
        self.assertFalse((p / "out" / "report.html").exists())
        self.assertFalse((p / "out" / "audit.tsv").exists())
        self.assertEqual(run(["test"], p).returncode, 0)

    def test_step_three_rolls_back_to_the_last_keep(self) -> None:
        # The record's third line must show the source after ast:restore.
        r = run(["demo"], self.d)
        lines = r.stdout.splitlines()
        self.assertIn("3 ROLLBACK  (define (guess) 2)    live (define (guess) 7)", lines)
        self.assertNotIn("live (define (guess) 2)", r.stdout)


if __name__ == "__main__":
    unittest.main()
