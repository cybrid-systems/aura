#!/usr/bin/env python3
"""Regression for #4874: vector-fill! must require the Mutate effect.

Production default (no AURA_SANDBOX) is Restricted with no Mutate grant for a
CLI program. #5195 lets a vector that has not been published into a workspace
be filled as a local build. A write attempt publishes the target: the fill
closure is installed after that attempt, and both names stay Mutate-gated.
A let-local vector is still not shared state.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AURA_BIN = ROOT / "build" / "aura"

SOURCE = r"""
(define v (vector 1 2 3))
(define (hit-set)
  (try (begin (vector-set! v 0 9) 'NO-ERROR) (catch (e) e)))
(display (list 'vector-set! (hit-set))) (newline)
(display (list 'ref (vector-ref v 0))) (newline)
(define (hit-fill)
  (try (begin (vector-fill! v 7) 'NO-ERROR) (catch (e) e)))
(display (list 'vector-fill! (hit-fill))) (newline)
(display (list 'after v)) (newline)
"""

LOCAL = r"""
(define (local)
  (let ((u (vector 1 2 3)))
    (vector-set! u 0 9)
    (vector-fill! u 7)
    u))
(display (local)) (newline)
"""


def _run(sandbox: str | None, source: str = SOURCE, timeout: float = 60.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
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
            "(vector-set! effect-denied: mutate not granted tenant=0 op=vector-set!)",
            p.stdout,
        )
        self.assertIn(
            "(vector-fill! effect-denied: mutate not granted tenant=0 op=vector-fill!)",
            p.stdout,
        )
        self.assertIn("(ref 1)", p.stdout)
        self.assertIn("(after #(1 2 3))", p.stdout)

    def test_local_vector_is_not_shared_state(self) -> None:
        p = _run(None, LOCAL)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("#(7 7 7)", p.stdout)

    def test_off_allows_vector_fill(self) -> None:
        p = _run("off")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(vector-fill! NO-ERROR)", p.stdout)
        self.assertIn("(after #(7 7 7))", p.stdout)


if __name__ == "__main__":
    unittest.main()
