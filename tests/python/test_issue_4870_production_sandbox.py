#!/usr/bin/env python3
"""Regression for #4870: production-default sandbox must run agent/scope bodies.

CI sets AURA_SANDBOX=off everywhere; a user who sets nothing got ok joins while
the body never ran. The soft orch boundary must not be treated as a real
mutation boundary by typed-entry commit readiness.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AURA_BIN = ROOT / "build" / "aura"


def _run(source: str, timeout: float = 60.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_SANDBOX", None)
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


class TestProductionSandbox4870(unittest.TestCase):
    def test_agent_and_scope_bodies_run(self) -> None:
        src = r"""
(define (hg h k d) (if (hash-has-key? h k) (hash-ref h k) d))
(define out (channel:create 4))
(define c 0)
(orch:spawn-agent "a1" (lambda () (channel:send out 'body-ran) (set! c 1) 42))
(define j (orch:agent-join "a1" :timeout-ms 2000))
(display (list 'join (hash-ref j "status") 'ok (hg j "ok" '-) 'chan (channel:try-recv out) 'c c)) (newline)
(orch:scope-spawn "k" (lambda () (channel:send out 'scope-ran) 7))
(define s (orch:scope-join-all :timeout-ms 2000))
(display (list 'scope (hash-ref s "status") 'errors (hash-ref s "errors") 'chan (channel:try-recv out))) (newline)
"""
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(join ok ok #t chan body-ran c 1)", p.stdout)
        self.assertIn("(scope ok errors () chan scope-ran)", p.stdout)

    def test_agent_ask_answers(self) -> None:
        src = r"""
(define (hg h k d) (if (hash-has-key? h k) (hash-ref h k) d))
(orch:spawn-agent "w" (lambda ()
  (let loop ((n 0))
    (let ((m (orch:agent-recv "w" :wait #f)))
      (if (eq? #t (hg m "ok" #f))
          (begin (orch:agent-reply (hg m "correlation-id" 0) "x") (loop (+ n 1)))
          (begin (fiber:yield) (loop n)))))) :attach-mailbox #t)
(define a (orch:agent-ask "w" "q" :timeout-ms 2000))
(display (list 'ask (hg a "ok" '?) (hg a "status" '?))) (newline)
"""
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(ask #t ok)", p.stdout)


if __name__ == "__main__":
    unittest.main()
