#!/usr/bin/env python3
"""Regression for #4909 (AC 同一个错误只报一次 + file:line:col).

Before: a top-level `(+ 1 "a")` in a program file printed the error twice —
a bare `error: type mismatch …` line from the IR executor, then the located
`error: F:L:C: type error: …` from the file runner. A primitive error such as
`(car 5)` under display printed `error: car: not a pair` with no position.
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

MODES = {"default": {}, "strict0": {"AURA_STRICT": "0"}, "force-soa": {"AURA_FORCE_SOA": "1"}}


def run(src: str, extra: dict[str, str]) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as t:
        f = Path(t) / "p.aura"
        f.write_text(src)
        env = dict(os.environ)
        for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_STRICT", "AURA_FORCE_SOA"):
            env.pop(k, None)
        env.update(extra)
        return subprocess.run([str(AURA_BIN), "p.aura"], cwd=t, capture_output=True, text=True, env=env, timeout=60)


class ReportedOnceWithLocation(unittest.TestCase):
    def check(self, src: str, needle: str, where: str) -> None:
        for mode, extra in MODES.items():
            with self.subTest(mode=mode):
                r = run(src, extra)
                self.assertNotEqual(r.returncode, 0)
                lines = [ln for ln in r.stderr.splitlines() if needle in ln]
                self.assertEqual(len(lines), 1, r.stderr)
                self.assertIn(f"p.aura:{where}:", lines[0], r.stderr)

    def test_top_level_type_error_once(self) -> None:
        self.check('(+ 1 "a")\n', "type mismatch", "1:2")

    def test_type_error_in_display_once(self) -> None:
        self.check('(display 0)\n(display (+ 1 "a"))\n', "type mismatch", "2:11")

    def test_primitive_error_located(self) -> None:
        self.check("(display (car 5))\n", "not a pair", "1:11")

    def test_cdr_error_in_display_located(self) -> None:
        self.check("(display 1)\n(display (cdr 5))\n", "not a pair", "2:11")

    def test_bare_primitive_error_located(self) -> None:
        self.check("(car 5)\n", "not a pair", "1:2")

    def test_discarded_error_in_body_still_reported_once(self) -> None:
        self.check('(define (f) (+ 1 "a") 2)\n(display (f))\n', "type mismatch", "1:14")

    def test_displayed_error_in_body_located(self) -> None:
        self.check("(define (g) (display (car 5)) 7)\n(display (g))\n", "not a pair", "1:23")


if __name__ == "__main__":
    unittest.main()
