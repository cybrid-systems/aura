#!/usr/bin/env python3
"""Regression for #4909: an error raised inside an IR-run function body is located there.

Before: with AURA_PIPELINE_STRICT=force-soa `(car z)` failing inside `f` was
reported at the outermost call `(f 5)`: the IR producer note resolved the
instruction's node id against the caller's flat, where it was out of range,
so nothing was recorded and the call result won.

Not covered yet (tracked on #4909): `(define r (f 5))` — the value-define
bridge still locates the error at the define / call form.
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

CALL = (
    "(define (g y)\n  (+ y 1))\n(define (f x)\n  (let ((z (* x 2)))\n    (car z)))\n(display (g 1))\n(newline)\n(f 5)\n"
)


class IrBodyLocation(unittest.TestCase):
    def run_prog(self, src: str, pipeline: str | None) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(src)
            env = dict(os.environ)
            env["AURA_SANDBOX"] = "off"
            env.pop("AURA_PIPELINE_STRICT", None)
            env.pop("AURA_KEEP_GOING", None)
            if pipeline:
                env["AURA_PIPELINE_STRICT"] = pipeline
            return subprocess.run(
                [str(AURA_BIN), "p.aura"], cwd=d, capture_output=True, text=True, env=env, timeout=120
            )

    def check(self, src: str, where: str) -> None:
        for pipeline in (None, "force-soa"):
            with self.subTest(pipeline=pipeline or "default"):
                r = self.run_prog(src, pipeline)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn(f"error: p.aura:{where}: type error: car: not a pair", r.stderr)
                self.assertEqual(r.stderr.count("car: not a pair"), 1, r.stderr)

    def test_call_statement(self) -> None:
        self.check(CALL, "5:6")


if __name__ == "__main__":
    unittest.main()
