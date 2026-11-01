#!/usr/bin/env python3
"""Regression test for issue #4854: long-running orch RSS growth.

Before the fix every one of these loops grew VmRSS linearly for the life of
the process (measured at the bug report on dc49853ab):

  * orch:spawn-agent + orch:agent-join   ~13-16 KB per pair
  * orch:scope-spawn + orch:scope-join-all ~11-16 KB per child
  * (eval <datum>)                         ~8.7 KB per call
  * channel:create + send + recv (+close)  ~0.8-1 KB per channel

Post-fix the growth is bounded (allocator high-water only):

  * agent   1200 pairs   -> ~0.1 MB total
  * scope   1200 children-> 2-4 MB (two glibc arena steps, then flat)
  * eval    3000 calls   -> ~0.1 MB total
  * channel 15000 cycles -> ~0.1 MB total

The retainers were:
  * Scheduler::owned_fibers_ kept every Done Fiber object (plus its heap
    bookkeeping) until ~Scheduler (#3905). The orch settle paths now release
    them through Scheduler::retire_done_fiber once the AgentHandle is
    Done-path-cleaned and its mailbox is detached (#4854).
  * AgentNameTable kept a Done-path-cleaned slot until the next resolve /
    put; the join settle point now retires it (retire_settled).
  * g_retained_eval only shrank at ~Evaluator (#4359). (eval datum) now
    retains the parsed flat/pool only when the program can build a closure
    (contains a Lambda node); a datum/arithmetic eval drops them on return.
  * channels_ was append-only; channel:close now releases the slot and
    channel:create recycles it.

The channel probe includes channel:close: an UNCLOSED channel is still a
live, addressable id (send/recv may use it later), so it is intentionally
not reclaimed. Close is the settle point.

These thresholds are ~2-4x the post-fix high-water and well below the old
linear rate (agent/scope 1200 iterations alone were ~20 MB before the fix).
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

BATCHES = 6
WARMUP = 50

_RSS = r"""

(define (rss-kb)
  (let ((s (read-file "/proc/self/status")))
    (let ((n (string-length s)) (i 0) (found -1))
      (while (lambda () (and (= found -1) (< i (- n 6))))
        (lambda ()
          (if (equal? (substring s i (+ i 6)) "VmRSS:") (set! found i) 0)
          (set! i (+ i 1))))
      (if (= found -1) 0
          (let ((k (+ found 6)) (v 0) (done #f))
            (while (lambda () (and (< k n) (or (= (string-ref s k) 32) (= (string-ref s k) 9))))
              (lambda () (set! k (+ k 1))))
            (while (lambda () (and (not done) (< k n)))
              (lambda ()
                (let ((c (string-ref s k)))
                  (if (and (>= c 48) (<= c 57))
                      (begin (set! v (+ (* v 10) (- c 48))) (set! k (+ k 1)))
                      (set! done #t)))))
            v)))))

(define *n* 0)
(define (work-batch k)
  (let ((i 0))
    (while (lambda () (< i k))
      (lambda ()
        __BODY__
        (set! i (+ i 1))))))

(work-batch __WARMUP__)
(gc-heap)
(define *base* (rss-kb))
(display "START ") (display *base*) (newline)
(define *b* 0)
(define *prev* *base*)
(while (lambda () (< *b* __BATCHES__))
  (lambda ()
    (work-batch __SIZE__)
    (set! *b* (+ *b* 1))
    (gc-heap)
    (let ((now (rss-kb)))
      (display "BATCH ") (display *b*) (display " ") (display now)
      (display " DELTA ") (display (- now *prev*)) (newline)
      (set! *prev* now))))
"""

_PROBES = {
    "agent": (
        # unique names: the pre-fix name table kept one Done husk per name
        """(let ((name (string-append "w" (number->string *n*))))
          (set! *n* (+ *n* 1))
          (orch:spawn-agent name (lambda () 1))
          (orch:agent-join name :timeout-ms 3000))""",
        200,
        8 * 1024,
    ),
    "scope": (
        """(let ((name (string-append "s" (number->string *n*))))
          (set! *n* (+ *n* 1))
          (orch:scope-spawn name (lambda () 1))
          (orch:scope-join-all))""",
        200,
        8 * 1024,
    ),
    "eval": (
        """(set! *n* (+ *n* 1))
        (eval (list '+ *n* 1 2 3 4 5 6 7 8 9 10))""",
        500,
        8 * 1024,
    ),
    "channel": (
        """(let ((c (channel:create 4)))
          (channel:send c "abcdefghij")
          (channel:recv c)
          (channel:close c))""",
        2500,
        6 * 1024,
    ),
}


def _render(body: str, size: int) -> str:
    return (
        _RSS.replace("__BODY__", body)
        .replace("__SIZE__", str(size))
        .replace("__WARMUP__", str(WARMUP))
        .replace("__BATCHES__", str(BATCHES))
    )


def _run_probe(source: str, timeout: float = 240.0) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
            stdin=subprocess.DEVNULL,
        )
    finally:
        os.unlink(path)


class TestIssue4854OrchRss(unittest.TestCase):
    def _growth(self, source: str) -> tuple[int, list[int]]:
        proc = _run_probe(source)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        start = None
        samples: list[int] = []
        for line in proc.stdout.splitlines():
            m = re.match(r"START (\d+)$", line)
            if m:
                start = int(m.group(1))
                continue
            m = re.match(r"BATCH \d+ (\d+) DELTA (-?\d+)$", line)
            if m:
                samples.append(int(m.group(2)))
        self.assertIsNotNone(start, f"no START line in probe output:\n{proc.stdout[-2000:]}")
        self.assertEqual(len(samples), BATCHES, f"expected {BATCHES} samples:\n{proc.stdout[-2000:]}")
        assert start is not None
        return start, samples

    def test_4854_growth_is_bounded(self) -> None:
        for name, (body, size, limit_kb) in _PROBES.items():
            with self.subTest(source=name):
                start, deltas = self._growth(_render(body, size))
                # Per-batch growth after the (already warmed) first batch.
                growth = sum(deltas[1:])
                self.assertLess(
                    growth,
                    limit_kb,
                    f"#4854 {name}: RSS grew {growth} KB over "
                    f"{(BATCHES - 1) * size} iterations (limit {limit_kb} KB, "
                    f"start {start} KB, deltas {deltas}). Linear retention is back.",
                )
                # A linear retainer shows a positive delta every batch; the
                # fixed shape is 0 for at least half of the post-warmup
                # batches (allocator high-water steps are isolated).
                flat = sum(1 for d in deltas[1:] if d == 0)
                self.assertGreaterEqual(
                    flat,
                    (BATCHES - 1) // 2,
                    f"#4854 {name}: only {flat}/{BATCHES - 1} post-warmup batches "
                    f"were flat (deltas {deltas}) — growth looks linear",
                )

    def test_4854_channel_close_recycles_slot(self) -> None:
        proc = _run_probe(
            """
(define a (channel:create 2))
(channel:send a "x")
(channel:recv a)
(channel:close a)
(display "closed-send ") (display (channel:send a "y")) (newline)
(define b (channel:create 2))
(display "reused ") (display (= a b)) (newline)
(channel:send b "z")
(display "new-recv ") (display (channel:recv b)) (newline)
""".strip(),
            timeout=30.0,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("closed-send #f", proc.stdout)
        # The closed slot is released and the id is recycled LIFO.
        self.assertIn("reused #t", proc.stdout)
        self.assertIn("new-recv z", proc.stdout)


if __name__ == "__main__":
    unittest.main()
