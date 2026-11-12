#!/usr/bin/env python3
"""Regression for #5174: a 100000-iteration named let under --jit.

The interpreter's named let is a loop. The JIT used to push one native
frame per iteration and SIGSEGV. A tail self-call reuses the frame, so
the value matches the interpreter. A real non-tail recursion past the
native ceiling refuses with a diagnostic instead of faulting.
AURA_SANDBOX stays unset. --jit is argv[1].
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

NAMED = "(display (let loop ((i 0) (acc 0)) (if (= i 100000) acc (loop (+ i 1) (+ acc i)))))"
# The recursive call is an argument of +, so it is not a tail call.
DEEP = "(display (letrec ((f (lambda (n) (if (= n 0) 0 (+ 1 (f (- n 1))))))) (f 100000)))"
# Mutual recursion goes through aura_closure_call, not the direct self-call.
MUTUAL = (
    "(define (even n) (if (= n 0) 1 (odd (- n 1)))) "
    "(define (odd n) (if (= n 0) 0 (even (- n 1)))) "
    "(display (odd 10000))"
)
DEPTH = "recursion depth exceeded (>2048)"


def run_aura(args: list[str], timeout: float = 45) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)
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


class TestIssue5174JitTailLoop(unittest.TestCase):
    def test_named_let_100000_matches_interpreter(self) -> None:
        interp = run_aura(["-e", NAMED])
        jit = run_aura(["--jit", NAMED])
        self.assertEqual(interp.returncode, 0, interp.stderr)
        self.assertEqual(jit.returncode, 0, jit.stderr)
        self.assertEqual(interp.stdout, "4999950000")
        self.assertEqual(jit.stdout, "4999950000")
        self.assertNotIn("AURA CRASH", jit.stderr)
        self.assertGreaterEqual(jit.returncode, 0)

    def assert_depth_refuse(self, program: str) -> None:
        jit = run_aura(["--jit", program])
        blob = jit.stdout + jit.stderr
        self.assertNotEqual(jit.returncode, -11, blob)
        self.assertNotIn("AURA CRASH", blob)
        self.assertNotIn("fiber=", blob)
        self.assertIn(DEPTH, jit.stderr, blob)
        self.assertNotEqual(jit.returncode, 0, blob)
        self.assertEqual(jit.stdout, "")

    def test_deep_nontail_refuses(self) -> None:
        self.assert_depth_refuse(DEEP)

    def test_deep_mutual_refuses(self) -> None:
        self.assert_depth_refuse(MUTUAL)


if __name__ == "__main__":
    unittest.main()
