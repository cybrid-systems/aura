#!/usr/bin/env python3
"""Regression for #4899 (reopened): every face reports a top-level runtime error.

The first #4899 fix left four gaps:

* force-soa (direct and --load) evaluates one top-level form at a time. A
  value define such as ``(define x (car 5))`` and, under --load, a call whose
  function body fails returned an error *value* that the per-form loop dropped:
  nothing was printed and the run exited 0.
* A ``while`` loop at the top level dropped an error raised in its body under
  force-soa direct (the lowering escaped body errors only inside a lambda).
* The whole-program face (default / strict=0, direct) located a tail error
  with no recorded location at 1:1 of the file. The direct resume then started
  again from form 2, so earlier output was printed twice.

Each case must now print a located diagnostic once, run each earlier form
exactly once, and exit non-zero, with AURA_SANDBOX unset and off.
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

STRICTS = ("", "0", "force-soa")
SANDBOXES = (None, "off")


def _run(source: str, direct: bool, strict: str, sandbox: str | None):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_SANDBOX", None)
    env.pop("AURA_PIPELINE_STRICT", None)
    if sandbox is not None:
        env["AURA_SANDBOX"] = sandbox
    if strict:
        env["AURA_PIPELINE_STRICT"] = strict
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


DEFINE_VALUE = '(define x (car 5))\n(display "after") (newline)\n'
FN_BODY = '(define (f) (car 5) 1)\n(f)\n(display "after") (newline)\n'
FN_BODY_MID = '(define (f) (car 5) 1)\n(display "x") (newline)\n(f)\n(display "after") (newline)\n'
ASSERT_FN = '(define (chk x) (if (< x 0) (error "neg") x))\n(chk -1)\n(display "after") (newline)\n'
WHILE_BODY = '(define i 0)\n(while (< i 3) (car i) (set! i (+ i 1)))\n(display "after") (newline)\n'
TAIL_AFTER_OUTPUT = '(display "A")\n(newline)\n(display "B")\n(newline)\n(car 5)\n'


class TestTopLevelErrorsEveryFace4899(unittest.TestCase):
    def _each(self):
        for sandbox in SANDBOXES:
            for strict in STRICTS:
                for direct in (True, False):
                    tag = f"sandbox={sandbox} strict={strict or 'default'} direct={direct}"
                    yield tag, direct, strict, sandbox

    def _assert_reported(self, src: str, needle: str) -> None:
        for tag, direct, strict, sandbox in self._each():
            p = _run(src, direct, strict, sandbox)
            self.assertNotEqual(p.returncode, 0, f"{tag}: {p.stdout!r} {p.stderr!r}")
            self.assertIn("error", p.stderr, f"{tag}: {p.stderr!r}")
            self.assertIn(needle, p.stderr, f"{tag}: {p.stderr!r}")
            self.assertNotIn("1:1:", p.stderr, f"{tag}: not located at file start: {p.stderr!r}")

    def test_value_define_error(self) -> None:
        self._assert_reported(DEFINE_VALUE, "car")

    def test_function_body_error(self) -> None:
        self._assert_reported(FN_BODY, "car")

    def test_error_call_in_function(self) -> None:
        self._assert_reported(ASSERT_FN, "neg")

    def test_while_body_error(self) -> None:
        self._assert_reported(WHILE_BODY, "car")

    def test_function_body_error_mid_file_runs_forms_once(self) -> None:
        for tag, direct, strict, sandbox in self._each():
            p = _run(FN_BODY_MID, direct, strict, sandbox)
            self.assertNotEqual(p.returncode, 0, f"{tag}: {p.stdout!r} {p.stderr!r}")
            self.assertEqual(p.stdout.count("x"), 1, f"{tag}: {p.stdout!r}")
            self.assertEqual(p.stderr.count("car: not a pair"), 1, f"{tag}: {p.stderr!r}")

    def test_tail_error_located_and_not_rerun(self) -> None:
        for tag, direct, strict, sandbox in self._each():
            p = _run(TAIL_AFTER_OUTPUT, direct, strict, sandbox)
            self.assertNotEqual(p.returncode, 0, f"{tag}: {p.stdout!r} {p.stderr!r}")
            self.assertEqual(p.stdout, "A\nB\n", f"{tag}: earlier forms run once: {p.stdout!r}")
            self.assertIn("5:2:", p.stderr, f"{tag}: {p.stderr!r}")
            self.assertEqual(p.stderr.count("car: not a pair"), 1, f"{tag}: {p.stderr!r}")


if __name__ == "__main__":
    unittest.main()
