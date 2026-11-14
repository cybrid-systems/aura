#!/usr/bin/env python3
"""Regression for #4893: force-soa direct run must see top-level macros.

59f1e0600 (#4892) ran every non-value-define top-level form under ForceSoa
and wrapped plain forms in (begin ...). A define-hygienic-macro / defmacro
registration was then dropped, and a later call to the macro was lowered
as an ordinary application: `cannot call: or2-h` / `unbound variable`.
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


def _run(source: str, direct: bool = True, strict: str = "force-soa"):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = strict
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


HYG = "(define-hygienic-macro (or2-h a b) (let ((t a)) (if t t b)))\n"


class TestForceSoaDirectMacros4893(unittest.TestCase):
    def _check(self, src: str, want: str) -> None:
        for direct in (True, False):
            for strict in ("force-soa", "0"):
                p = _run(src, direct=direct, strict=strict)
                tag = f"direct={direct} strict={strict}"
                self.assertEqual(p.returncode, 0, f"{tag}: {p.stderr}")
                self.assertEqual(p.stdout.strip(), want, tag)

    def test_hygienic_macro_toplevel_call(self) -> None:
        self._check(HYG + "(display (or2-h #f 3)) (newline)\n", "3")

    def test_defmacro_toplevel_call(self) -> None:
        self._check("(defmacro (m1 x) (list (quote +) x 1))\n(display (m1 2)) (newline)\n", "3")

    def test_macro_called_inside_function(self) -> None:
        self._check(HYG + "(define (f i) (or2-h #f i))\n(display (f 4)) (newline)\n", "4")

    def test_macro_in_begin_and_value_define(self) -> None:
        src = HYG + ("(define v (or2-h #f 7))\n(begin (display (list v (or2-h #f 5))) (newline))\n")
        self._check(src, "(7 5)")


if __name__ == "__main__":
    unittest.main()
