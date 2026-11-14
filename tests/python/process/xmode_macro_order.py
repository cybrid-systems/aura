#!/usr/bin/env python3
"""Regression for #4901: a macro call uses the definition before it.

The whole-file (default / STRICT=0) face collected every define-hygienic-macro
in the flat into one name -> definition map, so the file final definition
rewrote calls that preceded it: redefining a macro between two calls printed
1000 1000 instead of 11 1000. The static pass now keeps every definition in
node order and expands a call with the last definition written before it.
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
    if strict:
        env["AURA_PIPELINE_STRICT"] = strict
    else:
        env.pop("AURA_PIPELINE_STRICT", None)
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


REDEF = (
    "(define-hygienic-macro (hm x) (+ x 1))\n"
    "(display (hm 10)) (newline)\n"
    "(define-hygienic-macro (hm x) (* x 100))\n"
    "(display (hm 10)) (newline)\n"
)

WITH_BETWEEN = (
    "(define-hygienic-macro (hm2 x) (+ x 1))\n"
    "(display (hm2 10)) (newline)\n"
    '(display "between") (newline)\n'
    "(define-hygienic-macro (hm2 x) (* x 100))\n"
    "(display (hm2 10)) (newline)\n"
)


class TestMacroRedefinitionOrder4901(unittest.TestCase):
    def test_each_call_uses_the_previous_definition(self) -> None:
        for src, want in ((REDEF, ["11", "1000"]), (WITH_BETWEEN, ["11", "between", "1000"])):
            for strict in ("force-soa", "0", ""):
                for direct in (True, False):
                    p = _run(src, direct=direct, strict=strict)
                    tag = f"strict={strict or 'default'} direct={direct}"
                    self.assertEqual(p.returncode, 0, f"{tag}: {p.stderr}")
                    self.assertEqual(p.stdout.strip().splitlines(), want, tag)


if __name__ == "__main__":
    unittest.main()
