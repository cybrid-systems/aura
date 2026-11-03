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

    def test_direct_run_toplevel_while_stays_on_ir_4892(self) -> None:
        # Issue #4892: the form-by-form direct-run face ran every top-level
        # form under the permissive policy, and a lone `(while ...)` root hit
        # the registered-prim-root walker shortcut, so the loop ran in the
        # tree-walker (string_ref_walk_4320 bench 11ms -> 40ms on CI).
        src = """
(define i 0)
(define n 0)
(define s "aaaaaaaaaa")
(define (walker-takes)
  (let ((m (engine:metrics "query:soa-dirty-stats")))
    (- (hash-ref m "tree-walker-fallback-total")
       (hash-ref m "tree-walker-fallback-forced-soa-total"))))
(define w0 (walker-takes))
(while (< i 10) (begin (set! n (+ n (string-ref s i))) (set! i (+ i 1))))
(define w1 (walker-takes))
(display (list 'n n 'i i 'walker-takes (- w1 w0)))
(newline)
"""
        p = _run(src, direct=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip().splitlines(), ["(n 970 i 10 walker-takes 0)"])

    def test_direct_run_walker_bound_value_visible_to_ir_4892(self) -> None:
        # Issue #4892: a value define bound through the walker (fiber:join)
        # must be visible to a later Begin-wrapped IR form.
        src = """
(define c 0)
(define f (fiber:spawn (lambda () (set! c 11) 1)))
(define jr (fiber:join f))
(define k 0)
(while (< k 3) (set! k (+ k 1)))
(display (list 'jr jr 'c c 'k k (+ jr c k)))
(newline)
"""
        p = _run(src, direct=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip().splitlines(), ["(jr 1 c 11 k 3 15)"])

    def test_direct_run_wrapped_form_diag_column_4892(self) -> None:
        # Issue #4892: the Begin wrap must not shift the reported column.
        src = '(display "a")\n(newline)\n(foo 1)\n(display "after")\n(newline)\n'
        p = _run(src, direct=True)
        self.assertEqual(p.returncode, 1)
        self.assertIn("1:2:", p.stderr)
        self.assertIn("foo", p.stderr)
        self.assertEqual(p.stdout.strip().splitlines(), ["a", "after"])
