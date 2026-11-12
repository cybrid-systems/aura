#!/usr/bin/env python3
"""Regression for #5176: --jit display/write of compounds matches the interpreter.

A list, pair, vector, and hash must print as values. The raw heap word
(141 for a vector, 17 for a hash) is not the value. set! stays on the
mutate gate from #5173 and is not required to match.
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

# Pinned to the interpreter's display/write text. None means compare only.
CASES: list[tuple[str, str | None]] = [
    ("(display (list 1 2 3))", "(1 2 3)"),
    ("(display (cons 1 2))", "(1 . 2)"),
    ("(write (list 1 2 3))", "(1 2 3)"),
    ("(write (cons 1 2))", "(1 . 2)"),
    ("(display (cdr (list 1 2)))", "(2)"),
    ("(display (cons 1 (cons 2 ())))", "(1 2)"),
    ('(display (list "a" "b"))', "(a b)"),
    ('(write (list "a" "b"))', '("a" "b")'),
    ("(display (vector 1 2 3))", "#(1 2 3)"),
    ("(write (vector 1 2))", "#(1 2)"),
    ("(display (make-vector 2 7))", "#(7 7)"),
    ("(display (vector))", "#()"),
    ("(display (make-vector 0))", "#()"),
    ("(display (make-vector 2))", "#(() ())"),
    ('(display (vector "a" "b"))', "#(a b)"),
    ('(write (vector "a" "b"))', '#("a" "b")'),
    ('(display (vector (list "a" "b")))', "#((a b))"),
    ('(write (vector "a\\"b"))', '#("a\\"b")'),
    ("(display (vector (list 1 2) (vector 3)))", "#((1 2) #(3))"),
    ("(display (vector 1.5))", "#(1.5)"),
    ('(display (hash "k" 1))', "<hash[1]>"),
    ('(write (hash "k" 1))', "<hash[1]>"),
    ("(display (hash))", "<hash[0]>"),
    ('(display (hash "a" 1 "b" 2))', "<hash[2]>"),
    ("(display (make-hash))", "<hash[0]>"),
    ('(display (vector (hash "k" 1)))', "#(<hash[1]>)"),
    ("(display (car (list 1 2)))", "1"),
    ("(display (vector-length (vector 1 2 3)))", "3"),
    ("(display (vector-ref (vector 1 2 3) 1))", "2"),
    ("(let ((x (vector 1 2))) x)", None),
    ('(let ((x (hash "k" 1))) x)', None),
    ("(let ((x (list 1 2 3))) x)", None),
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


class TestIssue5176JitCompoundDisplay(unittest.TestCase):
    def test_jit_display_matches_interpreter(self) -> None:
        for src, expect in CASES:
            with self.subTest(src=src):
                interp = run_aura(["-e", src])
                jit = run_aura(["--jit", src])
                self.assertEqual(interp.returncode, 0, interp.stderr)
                self.assertEqual(jit.returncode, 0, jit.stderr + jit.stdout)
                self.assertEqual(jit.stdout, interp.stdout, src)
                self.assertNotIn("AURA CRASH", jit.stderr)
                if expect is not None:
                    self.assertEqual(jit.stdout, expect, src)
                self.assertNotIn(jit.stdout.strip(), {"141", "17", "129"})

    def test_write_file_matches_interpreter(self) -> None:
        src = '(display (write-file "wf.txt" "ok"))'
        interp = run_aura(["-e", src])
        jit = run_aura(["--jit", src])
        self.assertEqual(jit.returncode, interp.returncode, jit.stderr + interp.stderr)
        self.assertEqual(jit.stdout, interp.stdout)
        self.assertIn("effect-denied", interp.stderr)
        self.assertIn("effect-denied", jit.stderr)
        self.assertNotIn("AURA CRASH", jit.stderr)


if __name__ == "__main__":
    unittest.main()
