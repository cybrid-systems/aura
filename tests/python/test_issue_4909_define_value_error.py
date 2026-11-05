#!/usr/bin/env python3
"""Regression for #4909: an error value bound by a head-of-file define is never swallowed.

Before: when a file starts with several defines (the evaluator's letrec face),
`(define r (f 5))` where `f` returns an error value (`(car z)` on a non-pair)
stored the error in `r` and the file ran on: `after` was printed and the run
exited 0 with no diagnostic. A single define, or one after an expression,
already reported it. Also: force-soa said `cannot call: memq` without the
did-you-mean that the default face gives.
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

HEAD_DEFINES = '(define (f z)\n  (car z))\n(define r (f 5))\n(display "after")\n'


def run_prog(src: str, pipeline: str | None, keep_going: bool = False) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "p.aura").write_text(src)
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        for k in ("AURA_PIPELINE_STRICT", "AURA_KEEP_GOING", "AURA_PRESET"):
            env.pop(k, None)
        if pipeline:
            env["AURA_PIPELINE_STRICT"] = pipeline
        if keep_going:
            env["AURA_KEEP_GOING"] = "1"
        return subprocess.run([str(AURA_BIN), "p.aura"], cwd=d, capture_output=True, text=True, env=env, timeout=120)


class DefineValueError(unittest.TestCase):
    def test_head_define_error_value_stops(self) -> None:
        for pipeline in (None, "0", "force-soa"):
            with self.subTest(pipeline=pipeline or "default"):
                r = run_prog(HEAD_DEFINES, pipeline)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertNotIn("after", r.stdout, r.stdout + r.stderr)
                self.assertRegex(r.stderr, r"error: p\.aura:\d+:\d+: type error: car: not a pair")
                self.assertEqual(r.stderr.count("car: not a pair"), 1, r.stderr)

    def test_keep_going_reports_once_and_runs_on(self) -> None:
        r = run_prog(HEAD_DEFINES, None, keep_going=True)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("after", r.stdout, r.stdout + r.stderr)
        self.assertEqual(r.stderr.count("car: not a pair"), 1, r.stderr)

    def test_memq_hint_on_every_face(self) -> None:
        for pipeline in (None, "force-soa"):
            with self.subTest(pipeline=pipeline or "default"):
                r = run_prog("(display (memq 1 (list 1)))\n", pipeline)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("did you mean 'member'?", r.stderr, r.stderr)


if __name__ == "__main__":
    unittest.main()
