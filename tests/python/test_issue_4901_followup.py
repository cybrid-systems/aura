#!/usr/bin/env python3
"""Regression for #4901 (reopened): force-soa macro calls inside functions.

The first fix (static pass, node order) covered the whole-file faces. Under
AURA_PIPELINE_STRICT=force-soa every top-level form is evaluated in its own
flat, so the static pass never sees a macro defined by an earlier form; the
walker expands a function body's macro call lazily, at call time, with the
LATEST definition. A function defined between two definitions of the same
macro therefore printed (1000 1000) instead of (11 1000).

Each eval flat now pins the macro definitions that existed when its eval
finished; the walker expands a call in that flat with the last definition
registered at or before the pin. A forward reference (the macro did not
exist yet when the function was defined) still sees the latest definition,
exactly like the whole-file faces.
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

FACES = [(s, d) for s in ("force-soa", "0", "") for d in (True, False)]


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


CASES = {
    "fn_between_redefs": (
        "(define-hygienic-macro (hm x) (+ x 1))\n"
        "(define (use1) (hm 10))\n"
        "(define-hygienic-macro (hm x) (* x 100))\n"
        "(define (use2) (hm 10))\n"
        "(display (list (use1) (use2))) (newline)\n",
        "(11 1000)",
    ),
    "called_before_and_after": (
        "(define-hygienic-macro (hm x) (+ x 1))\n"
        "(define (use1) (hm 10))\n"
        "(display (use1)) (newline)\n"
        "(define-hygienic-macro (hm x) (* x 100))\n"
        "(display (list (use1) (hm 10))) (newline)\n"
        "(define (use2) (hm 10))\n"
        "(display (use2)) (newline)\n",
        "11\n(11 1000)\n1000",
    ),
    "recursive_fn": (
        "(define-hygienic-macro (sq x) (* x x))\n"
        "(define (f n) (if (= n 0) 0 (+ (sq n) (f (- n 1)))))\n"
        "(display (f 10)) (newline)\n"
        "(define-hygienic-macro (sq x) (+ x x))\n"
        "(display (f 10)) (newline)\n"
        "(define (g n) (sq n))\n"
        "(display (g 10)) (newline)\n",
        "385\n385\n20",
    ),
    "forward_reference_sees_latest": (
        "(define (use) (fw 10))\n"
        "(define-hygienic-macro (fw x) (+ x 1))\n"
        "(display (use)) (newline)\n"
        "(define-hygienic-macro (fw x) (* x 100))\n"
        "(display (use)) (newline)\n",
        "11\n1000",
    ),
}


class TestForceSoaMacroView4901(unittest.TestCase):
    def test_all_faces_agree(self) -> None:
        failures = []
        for name, (src, want) in CASES.items():
            for strict, direct in FACES:
                p = _run(src, direct, strict)
                tag = f"{name} strict={strict or 'default'} direct={direct}"
                if p.returncode != 0 or p.stdout.strip() != want:
                    failures.append(f"{tag}: rc={p.returncode} out={p.stdout!r} err={p.stderr[-300:]!r}")
        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main()
