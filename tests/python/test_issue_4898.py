#!/usr/bin/env python3
"""Regression for #4898: orch:run-workflow must not stall ~1000 ms per call.

parallel-intend runs a workflow's tasks on a per-call local Scheduler when
the caller is not on a fiber, then stop()s it and joins its IO thread.
Scheduler::stop() only cleared running_; an IO loop already parked in its
1000 ms epoll_wait never woke, so the join waited out the full timeout
(bursty: 0, 1 or all 10 calls of a run at ~1002 ms). A one-task call is
~2 ms; none may take anywhere near the old idle timeout.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

SRC = """(define slow 0)
(define worst 0)
(let loop ((i 0)) (when (< i 10)
  (let ((t0 (monotonic-ms)))
    (orch:run-workflow (vector (hash "tasks" (vector (lambda () i)))))
    (let ((d (- (monotonic-ms) t0)))
      (when (> d 500) (set! slow (+ slow 1)))
      (when (> d worst) (set! worst d))))
  (loop (+ i 1))))
(display (list 'slow-of-10 slow 'worst-ms worst)) (newline)
"""

RUNS = 8


class TestRunWorkflowNoStall4898(unittest.TestCase):
    def test_no_idle_timeout_stall(self) -> None:
        with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
            f.write(SRC)
            path = f.name
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        try:
            for entry in ("direct", "load"):
                for run in range(RUNS):
                    argv = [str(AURA_BIN)] + (["--load", path] if entry == "load" else [path])
                    p = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=120)
                    tag = f"{entry} run {run}: {p.stdout!r} {p.stderr[-400:]!r}"
                    self.assertEqual(p.returncode, 0, tag)
                    m = re.search(r"\(slow-of-10 (\d+) worst-ms (\d+)\)", p.stdout)
                    self.assertIsNotNone(m, tag)
                    self.assertEqual(int(m.group(1)), 0, tag)
                    self.assertLess(int(m.group(2)), 500, tag)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
