#!/usr/bin/env python3
"""Regression for #4903: top-level when / unless under force-soa direct.

`AURA_PIPELINE_STRICT=force-soa aura file.aura` evaluates top-level forms
one at a time on the IR (ForceSoa, #4892). `when` / `unless` are tree-walker
special forms the lowering did not know, so a top-level
`(when #t (display "a"))` was lowered as an ordinary call: the body ran
eagerly, then `when` itself was called -> "cannot call: when", rc=1. A
named-let loop guarded by `when` never stopped (the body and the recursive
call were evaluated regardless of the test).
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

FACES = [(s, d) for s in ("force-soa", "0", "") for d in (True, False)]


def _run(source: str, direct: bool, strict: str, sandbox: str | None = "off"):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    if sandbox is None:
        env.pop("AURA_SANDBOX", None)
    else:
        env["AURA_SANDBOX"] = sandbox
    if strict:
        env["AURA_PIPELINE_STRICT"] = strict
    else:
        env.pop("AURA_PIPELINE_STRICT", None)
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


CASES = [
    ('(when #t (display "a"))\n(newline)\n', "a"),
    ('(when #f (display "a"))\n(display "b")\n(newline)\n', "b"),
    ('(unless #f (display "a"))\n(unless #t (display "x"))\n(newline)\n', "a"),
    ('(let ((d 5)) (unless (< d 0) (display "a")) (display "b"))\n(newline)\n', "ab"),
    ('(begin (when #t (display "a")) (when #t (display "b")))\n(newline)\n', "ab"),
    ('(let () (when #t (display "a")) (unless #f (display "b")))\n(newline)\n', "ab"),
    ("(let loop ((i 0)) (when (< i 3) (display i) (loop (+ i 1))))\n(newline)\n", "012"),
    ("(display (list (when #t 1 2) (unless #f 3)))\n(newline)\n", "(2 3)"),
]


class TestTopLevelWhenUnless4903(unittest.TestCase):
    def test_when_unless_all_faces(self) -> None:
        for src, want in CASES:
            for strict, direct in FACES:
                for sandbox in ("off", None):
                    p = _run(src, direct, strict, sandbox)
                    tag = f"{src!r} strict={strict or 'default'} direct={direct} sb={sandbox}"
                    self.assertEqual(p.returncode, 0, f"{tag}: {p.stderr}")
                    self.assertNotIn("cannot call", p.stderr, tag)
                    self.assertEqual(p.stdout.strip(), want, tag)

    def test_named_let_loop_terminates(self) -> None:
        src = (
            "(define n 0)\n"
            "(let loop ((i 0)) (when (< i 20000) (set! n (+ n i)) (loop (+ i 1))))\n"
            "(display n)\n(newline)\n"
        )
        for strict, direct in FACES:
            p = _run(src, direct, strict)
            tag = f"strict={strict or 'default'} direct={direct}"
            self.assertEqual(p.returncode, 0, f"{tag}: {p.stderr}")
            self.assertEqual(p.stdout.strip(), "199990000", tag)

    def test_body_error_still_reported(self) -> None:
        src = '(when #t (car 5) (display "x"))\n(display "after")\n(newline)\n'
        for strict, direct in FACES:
            p = _run(src, direct, strict)
            tag = f"strict={strict or 'default'} direct={direct}"
            self.assertEqual(p.returncode, 1, tag)
            self.assertIn("car: not a pair", p.stderr, tag)
            self.assertNotIn("x", p.stdout, tag)


if __name__ == "__main__":
    unittest.main()
