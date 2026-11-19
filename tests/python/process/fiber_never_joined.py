#!/usr/bin/env python3
"""Regression for #4890: never-joined agents kept their checkpoint stacks.

A Done agent that is never joined kept its per-fiber mutation / yield
checkpoint stacks (~20 KB) until ~Fiber at join. They now go back to the
BoundedVectorPool at the Done edge (release_done_resources), so a
spawn-and-forget loop reuses pooled stacks instead of allocating one each.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def _run(source: str, extra_env: dict | None = None, timeout: float = 120.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    if extra_env:
        env.update(extra_env)
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


def _field(out: str, key: str) -> int:
    m = re.search(rf"{key}=(-?\d+)", out)
    if not m:
        raise AssertionError(f"{key} missing in output: {out!r}")
    return int(m.group(1))


_POOL = """
(define (pool k) (let ((h (stats:get "query:per-fiber-stack-pool-stats")))
  (if (hash-has-key? h k) (hash-ref h k) -1)))
"""


class TestNeverJoinedAgentStacks4890(unittest.TestCase):
    def test_done_agents_return_checkpoint_stacks(self) -> None:
        n = 600
        src = (
            _POOL
            + f"""
(define N {n})
(define la0 (pool "lazy-allocs"))
(define h0 (pool "pool-hits"))
(let loop ((r 0))
  (when (< r N)
    (orch:spawn-agent (format "nj4890-~a" r) (lambda () 1))
    (when (= 0 (modulo r 25)) (gc-heap))
    (loop (+ r 1))))
;; let the spawn backlog run (bodies Done, never joined). Wait until every
;; agent has acquired its mutation + yield stacks (2N acquires) and the Done
;; edge has returned them; an early sample measures the startup burst, not
;; the pool-reuse contract.
(define t0 (monotonic-ms))
(let wait ()
  (when (and (< (- (monotonic-ms) t0) 15000)
             (or (< (- (+ (pool "pool-hits") (pool "lazy-allocs")) (+ h0 la0)) (* 2 N))
                 (> (pool "live-stacks") 8)))
    (gc-heap) (wait)))
(display (format "hits-delta=~a lazy-delta=~a live-stacks=~a\\n"
                 (- (pool "pool-hits") h0)
                 (- (pool "lazy-allocs") la0)
                 (pool "live-stacks")))
"""
        )
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr[-2000:])
        hits = _field(p.stdout, "hits-delta")
        live = _field(p.stdout, "live-stacks")
        # Before: a never-joined agent held its stack until ~Fiber, so every
        # acquire missed the (empty) pool (hits ~ 0, lazy ~ 2N). After: the
        # Done edge returns them, so later agents reuse pooled stacks; only
        # the spawn burst can miss, so pin reuse (hits >= N) and the drained
        # live gauge rather than a raw miss count that tracks concurrency.
        self.assertGreaterEqual(hits, n, p.stdout)
        self.assertGreaterEqual(live, 0, p.stdout)
        self.assertLessEqual(live, 8, p.stdout)

    def test_joined_agent_still_reports(self) -> None:
        src = """
(orch:spawn-agent "j4890-ok" (lambda () 41))
(orch:spawn-agent "j4890-err" (lambda () (error "planned")))
(define a (orch:agent-join "j4890-ok" :timeout-ms 5000))
(define b (orch:agent-join "j4890-err" :timeout-ms 5000))
(display (format "ok=~a err=~a\\n" (hash-ref a "status") (hash-ref b "status")))
"""
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr[-2000:])
        self.assertIn("ok=ok", p.stdout)
        self.assertNotIn("err=ok", p.stdout)


if __name__ == "__main__":
    unittest.main()
