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
(let loop ((r 0))
  (when (< r N)
    (orch:spawn-agent (format "nj4890-~a" r) (lambda () 1))
    (when (= 0 (modulo r 25)) (gc-heap))
    (loop (+ r 1))))
;; let the spawn backlog run (bodies Done, never joined)
(define t0 (monotonic-ms))
(let wait ()
  (when (and (< (- (monotonic-ms) t0) 8000)
             (or (< (pool "live-stacks") 0) (> (pool "live-stacks") 8)))
    (gc-heap) (wait)))
(display (format "lazy-delta=~a live-stacks=~a\\n" (- (pool "lazy-allocs") la0) (pool "live-stacks")))
"""
        )
        p = _run(src)
        self.assertEqual(p.returncode, 0, p.stderr[-2000:])
        lazy = _field(p.stdout, "lazy-delta")
        live = _field(p.stdout, "live-stacks")
        # Before: every never-joined agent held its own stack (lazy-delta ~ N,
        # one pool miss per agent; no live-stacks gauge). After: the Done
        # edge returns them, so later agents hit the pool.
        self.assertLess(lazy, n // 4, p.stdout)
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
