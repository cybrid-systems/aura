#!/usr/bin/env python3
"""Regression for #5175: top-level let values under --jit match the interpreter.

The numeric body of a top-level let was already right. What still diverged
was the value the CLI printed (a let-bound string, a nested lambda's number
wrapped as a cell, a returned closure) and a few consumers of JIT pairs and
evaluator hashes (list-ref, apply, hash-ref, display of a list). set! stays
on the mutate gate from #5173 and is not required to match.
AURA_SANDBOX stays unset. --jit is argv[1].
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

# Interpreter and --jit must print the same stdout. A top-level let binding
# is never read back as 0.
CASES = [
    "(display (let ((x 5)) (+ x 1)))",
    "(let ((x 5)) (+ x 1))",
    "(let ((x 1)) x)",
    "(let ((x 1) (y 2)) (+ x y))",
    "(let ((x 1) (y 2) (z 3)) (+ x y z))",
    '(let ((x "s")) (string-length x))',
    "(let ((x 1)) (let ((y 2)) (+ x y)))",
    "(display (let ((x 2)) (let ((y 3)) (* x y))))",
    "(display (let ((x 2) (y 3) (z 4)) (+ x y z)))",
    "(let* ((x 1) (y (+ x 1))) y)",
    "(let* ((a 1) (b 2) (c (+ a b))) c)",
    '(let ((x "hi")) x)',
    '(let ((x "hi") (y "yo")) x)',
    '(let ((x "hi") (y "yo")) y)',
    '(let ((x 5)) "hi")',
    '(string-append "h" "i")',
    '(let ((x (string-append "h" "i"))) x)',
    "((lambda () (let ((x 5)) (+ x 1))))",
    "(define (g) (let ((x 5)) (+ x 1))) (g)",
    "(let ((f (lambda () 1))) f)",
    "(let ((x (cons 1 2))) x)",
    "(let ((x (list 1 2 3))) x)",
    "(list 1 2 3)",
    "(display (list 1 2 3))",
    "(display (cdr (list 1 2)))",
    "(display (cons 1 (cons 2 ())))",
    "(display (list-ref (list 1 2 3) 1))",
    "(display (list-ref (list 1 2 3) 0))",
    "(display (list-ref (list 1 2 3) 2))",
    "(display (apply + (list 1 2 3)))",
    "(display (apply + (list 4 5)))",
    "(display (hash-ref (hash 1 2) 1))",
    '(display (hash-ref (hash "k" 1) "k"))',
    '(display (let ((h (hash "k" 1))) (hash-ref h "k")))',
    "(display (let ((v (vector 1 2 3))) (vector-ref v 2)))",
    "(display (let ((f (lambda (x) (lambda (y) (+ x y))))) ((f 1) 2)))",
    "(let ((x #t)) x)",
    "(let ((x ())) x)",
    "(let ((x 1.5)) x)",
]


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


class TestIssue5175JitLetBinding(unittest.TestCase):
    def test_jit_values_match_interpreter(self) -> None:
        for src in CASES:
            with self.subTest(src=src):
                interp = run_aura(["-e", src])
                jit = run_aura(["--jit", src])
                self.assertEqual(interp.returncode, 0, interp.stderr)
                self.assertEqual(jit.returncode, 0, jit.stderr + jit.stdout)
                self.assertEqual(jit.stdout, interp.stdout, src)
                self.assertNotIn("AURA CRASH", jit.stderr)
                if src == "(let ((x 1)) x)":
                    self.assertEqual(jit.stdout.strip(), "1")
                    self.assertNotEqual(jit.stdout.strip(), "0")


if __name__ == "__main__":
    unittest.main()
