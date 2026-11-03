#!/usr/bin/env python3
"""Regression for #4874: vector-fill! must require the Mutate effect.

Production default (no AURA_SANDBOX) is Restricted with no Mutate grant for a
CLI program, so vector-set! / string-fill! already report effect-denied while
vector-fill! used to rewrite the vector. The sibling name was missing from
infer_required_effects_from_name (security_side_effect.hh), so dispatch
inferred kEffectNone and skipped the capability choke.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AURA_BIN = ROOT / "build" / "aura"

SOURCE = r"""
(define (t name thunk) (display (list name (try (begin (thunk) 'NO-ERROR) (catch (e) e)))) (newline))
(define v (vector 1 2 3))
(t 'vector-set!  (lambda () (vector-set! v 0 9)))
(t 'vector-fill! (lambda () (vector-fill! v 7)))
(display (list 'after v)) (newline)
"""


def _run(sandbox: str | None, timeout: float = 60.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(SOURCE)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_SANDBOX", None)
    if sandbox is not None:
        env["AURA_SANDBOX"] = sandbox
    env["AURA_PIPELINE_STRICT"] = "0"
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
    finally:
        os.unlink(path)


class TestVectorFillMutate4874(unittest.TestCase):
    def test_restricted_denies_vector_fill(self) -> None:
        p = _run(None)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn(
            "(vector-fill! effect-denied: mutate not granted tenant=0 op=vector-fill!)",
            p.stdout,
        )
        self.assertIn("(after #(1 2 3))", p.stdout)

    def test_off_allows_vector_fill(self) -> None:
        p = _run("off")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(vector-fill! NO-ERROR)", p.stdout)
        self.assertIn("(after #(7 7 7))", p.stdout)


if __name__ == "__main__":
    unittest.main()
