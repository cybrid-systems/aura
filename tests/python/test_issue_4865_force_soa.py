#!/usr/bin/env python3
"""Regression for #4865/#4842: force-soa closure side effects.

Inline lambda bodies must write the same top-level evaluator cell the
following forms read; fiber/agent bodies must see channel sends.
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


def _run(source: str, strict: str = "force-soa", timeout: float = 60.0, direct: bool = False):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = strict
    try:
        argv = [str(AURA_BIN)]
        if not direct:
            argv.append("--load")
        argv.append(path)
        return subprocess.run(
            argv,
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
    finally:
        os.unlink(path)


class TestForceSoaCells4865(unittest.TestCase):
    def test_fiber_set_and_channel_visible(self) -> None:
        src = """
(define c 0)
(define f (fiber:spawn (lambda () (set! c 11) 1)))
(define jr (fiber:join f))
(display (list 'fiber-sep jr c)) (newline)
(define out (channel:create 4))
(define f2 (fiber:spawn (lambda () (channel:send out 'x) 2)))
(define jr2 (fiber:join f2))
(display (list 'chan-sep jr2 (channel:try-recv out))) (newline)
"""
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(fiber-sep 1 11)", p.stdout)
        self.assertIn("(chan-sep 2 x)", p.stdout)

    def test_inline_lambda_set(self) -> None:
        src = """
(define c 0)
(display (list 'direct ((lambda () (set! c 1) 1)) c)) (newline)
(define cc 0)
(display (list 'apply (apply (lambda () (set! cc 2) 1) (list)) cc)) (newline)
"""
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("(direct 1 1)", p.stdout)
        self.assertIn("(apply 1 2)", p.stdout)

    def test_4842_scope_in_agent(self) -> None:
        src = """
(define out (channel:create 8))
(orch:spawn-agent "par" (lambda ()
  (orch:scope-spawn "k1" (lambda () (channel:send out 'k1-ran) 10))
  (let* ((t0 (monotonic-ms)) (r (orch:scope-join-all :timeout-ms 1000)))
    (channel:send out (list 'inner-ms (- (monotonic-ms) t0) 'status (hash-ref r "status"))))))
(define jp (orch:agent-join "par" :timeout-ms 4000))
(display (list (channel:try-recv out) (channel:try-recv out))) (newline)
(display (list 'outer-status (hash-ref jp "status"))) (newline)
"""
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("inner-ms", p.stdout)
        self.assertIn("status ok", p.stdout)
        self.assertIn("k1-ran", p.stdout)
        self.assertIn("(outer-status ok)", p.stdout)

    def test_direct_run_force_soa_side_effects(self) -> None:
        src = """
(define c 0)
(define f (fiber:spawn (lambda () (set! c 11) 1)))
(define jr (fiber:join f))
(display (list 'fiber-sep jr c)) (newline)
(define out (channel:create 4))
(define f2 (fiber:spawn (lambda () (channel:send out 'x) 2)))
(define jr2 (fiber:join f2))
(display (list 'chan-sep jr2 (channel:try-recv out))) (newline)
"""
        p = _run(src, direct=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip().splitlines(), ["(fiber-sep 1 11)", "(chan-sep 2 x)"])

    def test_direct_run_4842_scope_in_agent(self) -> None:
        src = """
(define out (channel:create 8))
(orch:spawn-agent "par" (lambda ()
  (orch:scope-spawn "k1" (lambda () (channel:send out 'k1-ran) 10))
  (let* ((t0 (monotonic-ms)) (r (orch:scope-join-all :timeout-ms 1000)))
    (channel:send out (list 'inner-ms (- (monotonic-ms) t0) 'status (hash-ref r "status"))))))
(define jp (orch:agent-join "par" :timeout-ms 4000))
(display (list (channel:try-recv out) (channel:try-recv out))) (newline)
(display (list 'outer-status (hash-ref jp "status"))) (newline)
"""
        p = _run(src, direct=True, timeout=90)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("k1-ran", p.stdout)
        self.assertIn("(outer-status ok)", p.stdout)
        self.assertNotIn("status timeout", p.stdout)
