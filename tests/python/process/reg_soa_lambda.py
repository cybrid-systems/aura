#!/usr/bin/env python3
"""Regression for #4895: force-soa per-form calls with a lambda argument.

The form-by-form force-soa face hit CompilerService::eval's workspace
call shortcut for `(f (lambda () 1))`: each argument was evaluated by a
nested eval() whose expression IRInterpreter owned the new closure and
died on return. f's body then called a dangling id, which collided with
f's own closure id (`arity mismatch: f expects 1 argument, got 0`) or
resolved nowhere (`eval_flat: invalid closure`).
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


def _run(source: str, direct: bool, strict: str):
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


CASES = [
    ("(define (f g) (display (g)) (newline))\n(f (lambda () 1))\n", "1"),
    ("(define (f g) (display (g 1)) (newline))\n(f (lambda (x) (+ x 10)))\n", "11"),
    ("(define (f g) (display (+ 1 (g))) (newline))\n(f (lambda () 1))\n", "2"),
    ("(define (f a g) (display (g a)) (newline))\n(f 4 (lambda (x) (* x x)))\n", "16"),
    (
        "(define (f g) (let ((r (g))) (display r)) (newline))\n(f (lambda () 1))\n"
        "(define (h g) (g) (display 9) (newline))\n(h (lambda () 1))\n",
        "1\n9",
    ),
]


class TestForceSoaLambdaArg4895(unittest.TestCase):
    def test_lambda_arg_called_in_non_tail_form(self) -> None:
        for src, want in CASES:
            for direct in (True, False):
                for strict in ("force-soa", "0"):
                    p = _run(src, direct, strict)
                    tag = f"direct={direct} strict={strict} src={src!r}"
                    self.assertEqual(p.returncode, 0, f"{tag}: {p.stderr}")
                    self.assertEqual(p.stdout.strip(), want, tag)
                    self.assertNotIn("invalid closure", p.stderr, tag)
                    self.assertNotIn("arity mismatch", p.stderr, tag)


if __name__ == "__main__":
    unittest.main()
