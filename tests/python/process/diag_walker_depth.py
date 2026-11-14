#!/usr/bin/env python3
"""Regression for #4909: a walker depth limit is not a program error, and
nothing runs twice.

A whole-file eval can run on the tree-walker, whose C-stack guard stops a
non-tail recursion at 512 frames. The old driver hid that error and re-ran
every form from the start, so output before the failure appeared twice. Now
the earlier procedure defines are re-bound on the per-form (IR) path and the
run resumes AT the failing form: every line prints once, rc=0, no error.
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

PROG = (
    "(define (f n) (if (= n 0) 0 (+ 1 (f (- n 1)))))\n"
    '(display "start") (newline)\n'
    "(display (f 400)) (newline)\n"
    "(display (f 3000)) (newline)\n"
    '(display "end") (newline)\n'
)


class WalkerDepth(unittest.TestCase):
    def _run(self, strict, stdin_mode):
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env.pop("AURA_SOFT", None)
        env.pop("AURA_IR_MAX_CALL_DEPTH", None)
        if strict is None:
            env.pop("AURA_PIPELINE_STRICT", None)
        else:
            env["AURA_PIPELINE_STRICT"] = strict
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "deep.aura"
            p.write_text(PROG)
            if stdin_mode:
                return subprocess.run(
                    [str(AURA_BIN)], input=PROG, capture_output=True, text=True, env=env, cwd=td, timeout=120
                )
            return subprocess.run([str(AURA_BIN), str(p)], capture_output=True, text=True, env=env, cwd=td, timeout=120)

    def test_each_line_once(self):
        for strict in (None, "0", "1"):
            for stdin_mode in (False, True):
                with self.subTest(strict=strict, stdin=stdin_mode):
                    r = self._run(strict, stdin_mode)
                    self.assertEqual(r.stdout, "start\n400\n3000\nend\n", r.stdout + r.stderr)
                    self.assertNotIn("recursion depth", r.stderr)
                    self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
