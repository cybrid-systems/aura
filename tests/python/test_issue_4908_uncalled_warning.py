#!/usr/bin/env python3
"""Regression for #4908: force-soa does not warn about a value it never prints.

Before: the form-by-form force-soa face formatted every top-level form's
value, and formatting a closure prints "⚠ program returned an uncalled
function". `(set-code …) (eval-current) (display "ok")` therefore warned
under AURA_PIPELINE_STRICT=force-soa only. A program whose LAST value is a
procedure must still warn, on every face.
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

WARN = "program returned an uncalled function"


def run(prog: str, strict: str | None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
        env.pop(k, None)
    env["AURA_SANDBOX"] = "off"
    if strict:
        env["AURA_PIPELINE_STRICT"] = strict
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "p.aura").write_text(prog)
        return subprocess.run(
            [str(AURA_BIN), "p.aura"],
            cwd=d,
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )


class UncalledWarning(unittest.TestCase):
    def test_intermediate_closure_is_silent(self) -> None:
        prog = '(set-code "(define (s) 1)")\n(eval-current)\n(display "ok") (newline)\n'
        for strict in (None, "0", "force-soa"):
            with self.subTest(strict=strict):
                r = run(prog, strict)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("ok", r.stdout)
                self.assertNotIn(WARN, r.stderr)

    def test_last_closure_still_warns(self) -> None:
        prog = "(define (f x) x)\n(display 1) (newline)\nf\n"
        for strict in (None, "0", "force-soa"):
            with self.subTest(strict=strict):
                r = run(prog, strict)
                self.assertIn(WARN, r.stderr)
                self.assertIn("#<procedure>", r.stdout)


if __name__ == "__main__":
    unittest.main()
