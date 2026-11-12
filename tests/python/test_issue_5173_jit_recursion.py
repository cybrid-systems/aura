#!/usr/bin/env python3
"""Regression for #5173: --jit matches the interpreter on recursive code.

Default Restricted, session-less exec_jit used to drop closure captures
and binding cell inits (no Mutate grant). A letrec countdown printed 0,
fib printed 0, and (f 3 100) printed 5050. Compare aura -e with
aura --jit in separate processes. AURA_SANDBOX stays unset. --jit is
argv[1].
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

COUNTDOWN = "(display (letrec ((f (lambda (n) (if (= n 0) 0 (+ 1 (f (- n 1))))))) (f 10)))"
BASE5 = "(display (letrec ((f (lambda (n) (if (= n 0) 5 (+ n (f (- n 1))))))) (f 10)))"
FIB = "(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))) (display (fib 10))"
ACC = "(display (letrec ((f (lambda (a b) (if (= a 0) b (+ a (f (- a 1) b)))))) (f 3 100)))"
ACC_INC = "(display (letrec ((f (lambda (a b) (if (= a 0) b (+ a (f (- a 1) (+ b 1))))))) (f 3 100)))"
FACT = "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (display (fact 5))"
NAMED = "(display (let f ((n 4) (a 1)) (if (= n 0) a (f (- n 1) (* a n)))))"
ODD = "(define (even n) (if (= n 0) 1 (odd (- n 1)))) (define (odd n) (if (= n 0) 0 (even (- n 1)))) (display (odd 5))"
EVEN = (
    "(define (even n) (if (= n 0) 1 (odd (- n 1)))) (define (odd n) (if (= n 0) 0 (even (- n 1)))) (display (even 6))"
)
CAPTURE = "(display ((lambda (x) ((lambda () x))) 7))"
LET_CELL = "(display (let ((x 7)) x))"
CLOSED_K = "(display (let ((k 5)) (letrec ((f (lambda (n) (if (= n 0) k (+ 1 (f (- n 1))))))) (f 3))))"
IDENTITY = "(display ((lambda (n) n) 10))"
LET_FN = "(display (let ((f (lambda (n) (+ n 1)))) (f 10)))"

CASES = (
    ("countdown", COUNTDOWN, "10"),
    ("base5", BASE5, "60"),
    ("fib", FIB, "55"),
    ("accumulator", ACC, "106"),
    ("accumulator-inc", ACC_INC, "109"),
    ("fact", FACT, "120"),
    ("named-let", NAMED, "24"),
    ("odd", ODD, "1"),
    ("even", EVEN, "1"),
    ("capture", CAPTURE, "7"),
    ("let-cell", LET_CELL, "7"),
    ("closed-constant", CLOSED_K, "8"),
    ("identity", IDENTITY, "10"),
    ("let-fn", LET_FN, "11"),
)


def run_aura(
    args: list[str], extra: dict[str, str] | None = None, timeout: float = 45
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    if extra:
        env.update(extra)
    with tempfile.TemporaryDirectory() as tmp:
        return subprocess.run(
            [str(AURA_BIN), *args],
            cwd=tmp,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )


class TestIssue5173JitRecursion(unittest.TestCase):
    def assert_jit_matches(self, program: str, expect: str, extra: dict[str, str] | None = None) -> None:
        interp = run_aura(["-e", program], extra)
        jit = run_aura(["--jit", program], extra)
        ib = interp.stdout + interp.stderr
        jb = jit.stdout + jit.stderr
        self.assertEqual(interp.returncode, 0, ib)
        self.assertEqual(jit.returncode, 0, jb)
        self.assertEqual(interp.stdout, expect, ib)
        self.assertEqual(jit.stdout, expect, jb)
        self.assertEqual(jit.stdout, interp.stdout, jb)

    def test_recursive_shapes_match_the_interpreter(self) -> None:
        for name, program, expect in CASES:
            with self.subTest(name=name):
                self.assert_jit_matches(program, expect)

    def test_l1_off_still_matches(self) -> None:
        extra = {"AURA_JIT_L1": "0"}
        for name, program, expect in (
            ("countdown", COUNTDOWN, "10"),
            ("fact", FACT, "120"),
            ("accumulator", ACC, "106"),
            ("fib", FIB, "55"),
        ):
            with self.subTest(name=name):
                self.assert_jit_matches(program, expect, extra)


if __name__ == "__main__":
    unittest.main()
