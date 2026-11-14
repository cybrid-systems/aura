#!/usr/bin/env python3
r"""Regression for #5024: \`aura new agent\` prints the two roles' dialogue.

Before: the workflow computed three squares and printed only
"stages ok 2, failed 0"; the results were dropped and no role was visible.
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
    "propose  (define (double x) (+ x x))\n"
    "check    (double 21) => 42  KEEP\n"
    "propose  (define (double x) (+ x 1))\n"
    "check    (double 21) => 22  REJECT\n"
    "live     (define (double x) (+ x x))\n"
)


class Dialogue(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_demo_agent(self) -> None:
        r = run(["demo", "agent"], self.d)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout, EXPECTED)
        self.assertEqual(list(self.d.iterdir()), [])

    def test_new_agent_matches_demo(self) -> None:
        r = run(["new", "agent", "demo"], self.d)
        self.assertEqual(r.returncode, 0, r.stderr)
        p = self.d / "demo"
        ran = run(["run"], p)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout, EXPECTED)
        self.assertNotIn("stages ok", ran.stdout)
        self.assertEqual(run(["test"], p).returncode, 0)

    def test_rejected_definition_is_not_live(self) -> None:
        r = run(["demo", "agent"], self.d)
        lines = r.stdout.splitlines()
        self.assertEqual(lines[-1], "live     (define (double x) (+ x x))")
        self.assertNotIn("live     (define (double x) (+ x 1))", lines)


if __name__ == "__main__":
    unittest.main()
