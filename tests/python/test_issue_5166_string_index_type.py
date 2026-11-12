#!/usr/bin/env python3
"""Regression for #5166: string-ref and substring reject a non-integer index.

as_int's hot contract used to abort the process (SIGABRT) when the index
was a string, float, bool, list, or vector. Both primitives now return a
catchable type error that names the argument. Integer indexes, including
out-of-range and negative ones, keep their previous results. AURA_SANDBOX
stays unset and the run does not pass --preset, matching an ordinary
`aura -e` invocation.
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

REF_TYPE = "string-ref: index must be an integer"
SUB_START = "substring: start index must be an integer"
SUB_END = "substring: end index must be an integer"
REF_OOB = "string-ref: index out of bounds"

PROG = r"""
(define (show tag thunk)
  (let ((r (try (thunk) (catch e e))))
    (display tag)
    (display " ")
    (display r)
    (newline)))
(show "REF-STR" (lambda () (string-ref "abc" "x")))
(show "REF-FLT" (lambda () (string-ref "abc" 1.5)))
(show "REF-ONE" (lambda () (string-ref "abc" 1.0)))
(show "REF-BOOL" (lambda () (string-ref "abc" #f)))
(show "REF-LIST" (lambda () (string-ref "abc" (list 1))))
(show "REF-VEC" (lambda () (string-ref "abc" (vector 1))))
(show "SUB-START-STR" (lambda () (substring "abc" "x" 1)))
(show "SUB-END-STR" (lambda () (substring "abc" 0 "x")))
(show "SUB-START-FLT" (lambda () (substring "abc" 1.5 2)))
(show "SUB-END-BOOL" (lambda () (substring "abc" 0 #f)))
(show "SUB-START-LIST" (lambda () (substring "abc" (list 1) 2)))
(show "SUB-END-VEC" (lambda () (substring "abc" 0 (vector 1))))
(show "REF-OK" (lambda () (string-ref "abc" 1)))
(show "SUB-OK" (lambda () (substring "abc" 0 2)))
(show "REF-NEG" (lambda () (string-ref "abc" -1)))
(show "REF-OOB" (lambda () (string-ref "abc" 9)))
(show "SUB-SHORT" (lambda () (substring "abc" "x")))
(show "SUB-NONSTR" (lambda () (substring 1 "x" 1)))
"""

UNCAUGHT = (
    ('(string-ref "abc" "x")', REF_TYPE),
    ('(string-ref "abc" 1.5)', REF_TYPE),
    ('(substring "abc" "x" 1)', SUB_START),
    ('(substring "abc" 0 "x")', SUB_END),
)


def _env() -> dict[str, str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
        env.pop(key, None)
    return env


class TestIssue5166StringIndexType(unittest.TestCase):
    def test_wrong_typed_index_is_catchable(self) -> None:
        env = _env()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p.aura"
            path.write_text(PROG, encoding="utf-8")
            proc = subprocess.run(
                [str(AURA_BIN), "--quiet", str(path)],
                cwd=tmp,
                env=env,
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
        out = proc.stdout
        err = proc.stderr
        self.assertEqual(proc.returncode, 0, f"rc={proc.returncode}\nstdout={out!r}\nstderr={err!r}")
        self.assertNotIn("AURA CRASH", err)
        self.assertNotIn("SIGABRT", err)
        self.assertNotIn("SIGSEGV", err)
        for tag in ("REF-STR", "REF-FLT", "REF-ONE", "REF-BOOL", "REF-LIST", "REF-VEC"):
            self.assertIn(f"{tag} {REF_TYPE}", out)
        for tag in ("SUB-START-STR", "SUB-START-FLT", "SUB-START-LIST"):
            self.assertIn(f"{tag} {SUB_START}", out)
        for tag in ("SUB-END-STR", "SUB-END-BOOL", "SUB-END-VEC"):
            self.assertIn(f"{tag} {SUB_END}", out)
        self.assertIn("REF-OK 98", out)
        self.assertIn("SUB-OK ab", out)
        self.assertIn(f"REF-NEG {REF_OOB}", out)
        self.assertIn(f"REF-OOB {REF_OOB}", out)
        self.assertIn("SUB-SHORT 0", out)
        self.assertIn("SUB-NONSTR 0", out)

    def test_uncaught_cli_is_a_type_error(self) -> None:
        env = _env()
        for expr, message in UNCAUGHT:
            with self.subTest(expr=expr):
                proc = subprocess.run(
                    [str(AURA_BIN), "-e", expr],
                    cwd=ROOT,
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=120,
                    check=False,
                )
                blob = proc.stdout + proc.stderr
                self.assertEqual(proc.returncode, 1, blob)
                self.assertNotIn("AURA CRASH", blob)
                self.assertNotIn("SIGABRT", blob)
                self.assertIn(f"type error: {message}", proc.stderr)


if __name__ == "__main__":
    unittest.main()
