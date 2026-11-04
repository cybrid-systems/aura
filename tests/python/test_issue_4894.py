#!/usr/bin/env python3
"""Regression for #4894: force-soa hygienic macro calls must be O(N).

A hygienic macro registered by an earlier top-level form is not in the
calling flat's static macro_expand_all pass, so the walker expanded it at
run time. Every call cloned the macro body into the flat, inner-expanded it
and restamped every node of the flat. The flat grew by one body per call, so
N calls cost O(N^2): 1000 calls ~400ms, 4000 calls ~6s (STRICT=0: 4/15ms).
The walker now reuses a call site's expansion while the macro registry, the
macro body and the call node are unchanged.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

HYG = "(define-hygienic-macro (or2-h a b) (let ((t a)) (if t t b)))\n"


def _run(source: str, direct: bool, strict: str = "force-soa"):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = strict
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=120)
    finally:
        os.unlink(path)


def _loop_src(n: int) -> str:
    return (
        HYG
        + "(define (f i) (or2-h #f i))\n"
        + "(define t0 (monotonic-ms))\n"
        + f"(define (loop i acc) (if (< i {n}) (loop (+ i 1) (+ acc (f i))) acc))\n"
        + "(display (list (quote sum) (loop 0 0) (quote ms) (- (monotonic-ms) t0))) (newline)\n"
    )


class TestForceSoaHygienicMacroLinear4894(unittest.TestCase):
    def _ms(self, n: int, direct: bool) -> int:
        p = _run(_loop_src(n), direct=direct)
        self.assertEqual(p.returncode, 0, p.stderr)
        m = re.search(r"\(sum (\d+) ms (\d+)\)", p.stdout)
        self.assertIsNotNone(m, p.stdout + p.stderr)
        self.assertEqual(int(m.group(1)), n * (n - 1) // 2)
        return int(m.group(2))

    def test_calls_scale_linearly(self) -> None:
        for direct in (False, True):
            small = min(self._ms(1000, direct) for _ in range(2))
            big = min(self._ms(4000, direct) for _ in range(2))
            tag = f"direct={direct} 1000={small}ms 4000={big}ms"
            # Before: 1000 ~400ms, 4000 ~6200ms (x15). Linear is x4.
            self.assertLess(big, 1000, tag)
            self.assertLess(big, max(small, 5) * 8 + 50, tag)


class TestMemoKeepsSemantics4894(unittest.TestCase):
    def _check(self, src: str, want: str, stricts=("force-soa", "0")) -> None:
        for direct in (True, False):
            for strict in stricts:
                p = _run(src, direct=direct, strict=strict)
                tag = f"direct={direct} strict={strict}"
                self.assertEqual(p.returncode, 0, f"{tag}: {p.stderr}")
                self.assertEqual(p.stdout.strip(), want, tag)

    def test_repeated_calls_see_fresh_arguments(self) -> None:
        src = HYG + ("(define (f x) (or2-h #f x))\n(display (list (f 1) (f 2) (f (quote a)))) (newline)\n")
        self._check(src, "(1 2 a)")

    def test_hygiene_kept_across_calls(self) -> None:
        # The macro's `t` must not capture the caller's `t`.
        src = HYG + ("(define (g t) (or2-h #f t))\n(display (list (g 5) (g 6))) (newline)\n")
        self._check(src, "(5 6)")

    def test_redefinition_is_seen(self) -> None:
        src = (
            "(define-hygienic-macro (m x) (+ x 1))\n"
            "(define (f x) (m x))\n"
            "(display (f 1)) (newline)\n"
            "(define-hygienic-macro (m x) (* x 10))\n"
            "(display (f 1)) (newline)\n"
            "(define (g x) (m x))\n"
            "(display (list (g 1) (m 1))) (newline)\n"
        )
        # The memo is keyed on the registry epoch, so a redefinition is not
        # served from a stale expansion: code written after it (g, the
        # top-level call) sees the new macro. Issue #4901: f was defined
        # before the redefinition and keeps the definition in effect then,
        # on force-soa as on the whole-file face.
        self._check(src, "2\n2\n(10 10)")

    def test_nested_macros_and_two_call_sites(self) -> None:
        src = (
            "(define-hygienic-macro (inc x) (+ x 1))\n"
            "(define-hygienic-macro (inc2 x) (inc (inc x)))\n"
            "(define (f i) (list (inc2 i) (inc i) (inc2 (* i 2))))\n"
            "(display (list (f 1) (f 10))) (newline)\n"
        )
        self._check(src, "((3 2 4) (12 11 22))")


if __name__ == "__main__":
    unittest.main()
