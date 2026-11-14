#!/usr/bin/env python3
"""Regression for #4764: a nested define inside a workspace value define.

The internal loop must be captured from its own pre-bound cell, not from a
stale workspace value-cell entry of the same name.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AURA_BIN = ROOT / "build" / "aura"


class TestNestedDefine4764(unittest.TestCase):
    def test_inner_define_loop_runs(self) -> None:
        src = r"""
(set-code "(define cntd (lambda (f xs) (define (lp xs n) (if (null? xs) n (lp (cdr xs) (if (f (car xs)) (+ n 1) n)))) (lp xs 0)))")
(eval-current)
(display (cntd (lambda (a) (> a 1)) (list 1 2 3))) (newline)
"""
        with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
            f.write(src)
            path = f.name
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "0"
        try:
            p = subprocess.run(
                [str(AURA_BIN), "--load", path],
                capture_output=True,
                text=True,
                env=env,
                timeout=60,
            )
        finally:
            os.unlink(path)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), "2")


if __name__ == "__main__":
    unittest.main()
