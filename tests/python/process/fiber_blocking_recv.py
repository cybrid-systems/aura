#!/usr/bin/env python3
"""Regression for #4871: a blocking agent-recv worker must exit cleanly.

The worker handled one ask/reply then blocked in agent-recv :wait #t. Teardown
must cancel that recv, not wait out its 3s timeout and then crash in
fiber_slab_mint.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AURA_BIN = ROOT / "build" / "aura"


def _run(source: str, timeout: float = 20.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    try:
        started = time.monotonic()
        p = subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
        return p, time.monotonic() - started
    finally:
        os.unlink(path)


class TestBlockingRecvExit4871(unittest.TestCase):
    def test_worker_exit_is_clean_and_prompt(self) -> None:
        src = r"""
(define (hg h k d) (if (hash-has-key? h k) (hash-ref h k) d))
(orch:spawn-agent "w" (lambda ()
  (let loop ((n 0))
    (let ((m (orch:agent-recv "w" :wait #t :timeout-ms 3000)))
      (if (eq? #t (hg m "ok" #f))
          (begin (orch:agent-reply (hg m "correlation-id" 0) "x") (loop (+ n 1)))
          'recv-timeout)))) :attach-mailbox #t)
(define a (orch:agent-ask "w" "q" :timeout-ms 2000))
(display (list 'ask (hg a "ok" '?))) (newline)
"""
        for i in range(3):
            p, elapsed = _run(src)
            self.assertEqual(p.returncode, 0, f"run {i}: {p.stderr}")
            self.assertIn("(ask #t)", p.stdout)
            self.assertNotIn("AURA CRASH", p.stderr)
            # The worker recv timeout is 3000ms. A clean cancel lets the
            # process exit well before it.
            self.assertLess(elapsed, 2.5, f"run {i}: exit took {elapsed:.2f}s")


if __name__ == "__main__":
    unittest.main()
