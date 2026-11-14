#!/usr/bin/env python3
"""Regression for #4876: production (Restricted) macro deny must be actionable.

With AURA_SANDBOX unset, user macros (defmacro / define-hygienic-macro /
macros inside eval) are refused because aura soft does not grant
MacroSelfEvo. The refusal surfaced as the internal sentinel
`internal error: capability-deny` (and, on the force-soa per-form face, a
cascade of `unbound variable: <macro>` errors for every later use). The
user must get one actionable `effect-denied: macro-self-evo not granted`
message naming the escape hatch, and the sentinel must not leak.
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

MSG = "effect-denied: macro-self-evo not granted"


def _run(source: str, direct: bool, strict: str, sandbox: str | None = None):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_SANDBOX", None)
    if sandbox is not None:
        env["AURA_SANDBOX"] = sandbox
    env["AURA_PIPELINE_STRICT"] = strict
    argv = [str(AURA_BIN)] + ([] if direct else ["--load"]) + [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


DEFMACRO = "(defmacro (m1 x) (list (quote +) x 1))\n(display (m1 2)) (newline)\n"
HYG = (
    "(define-hygienic-macro (swap! a b) (let ((tmp a)) (set! a b) (set! b tmp)))\n"
    "(define x 1) (define y 2)\n(swap! x y)\n(display (list x y)) (newline)\n"
    "(swap! x y)\n(display (list x y)) (newline)\n"
)
EVAL_TRY = (
    "(display (try (eval '(begin (define-hygienic-macro (or2-h a b) "
    "(let ((t a)) (if t t b))) (or2-h #f 3))) "
    '(catch (e) (list \'caught (format "~a" e))))) (newline)\n'
)
MODES = [(d, s) for d in (True, False) for s in ("force-soa", "0")]


class TestMacroDenyActionable4876(unittest.TestCase):
    def test_toplevel_macro_deny_actionable(self) -> None:
        for name, src in (("defmacro", DEFMACRO), ("hygienic", HYG)):
            for direct, strict in MODES:
                p = _run(src, direct, strict)
                tag = f"{name} direct={direct} strict={strict}: {p.stderr!r}"
                self.assertNotEqual(p.returncode, 0, tag)
                self.assertIn(MSG, p.stderr, tag)
                self.assertIn("AURA_SANDBOX=off", p.stderr, tag)
                self.assertNotIn("capability-deny", p.stderr, tag)
                self.assertNotIn("internal error", p.stderr, tag)
                self.assertNotIn("unbound variable", p.stderr, tag)
                self.assertEqual(p.stderr.count(MSG), 1, tag)

    def test_eval_macro_deny_caught_actionable(self) -> None:
        for direct, strict in MODES:
            p = _run(EVAL_TRY, direct, strict)
            tag = f"direct={direct} strict={strict}: {p.stdout!r} {p.stderr!r}"
            self.assertIn("(caught", p.stdout, tag)
            self.assertIn(MSG, p.stdout, tag)
            self.assertNotIn("capability-deny", p.stdout, tag)

    def test_sandbox_off_macros_work(self) -> None:
        for direct, strict in MODES:
            p = _run(DEFMACRO, direct, strict, sandbox="off")
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.strip(), "3")
            p = _run(HYG, direct, strict, sandbox="off")
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.split(), ["(2", "1)", "(1", "2)"])


if __name__ == "__main__":
    unittest.main()
