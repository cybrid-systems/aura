#!/usr/bin/env python3
"""Issue #4877: --serve-async-bench must not make orch:agent-ask slow.

Repro: the #4831 regression program — an agent body polling its mailbox
(agent-recv :wait #f + fiber:yield) while the bench fiber issues 100
orch:agent-ask calls with :timeout-ms 2000 — had to finish inside a 30 s
KILL and timed out in 6/10 runs under the production sandbox.

Root causes measured on e744951e8:

  * 14 workers were created although the Soft Ready banner announced
    "Auto workers=1" (Scheduler auto-sized num_workers<=0).
  * Every reply wake is a foreign enqueue: it is staged in the worker's
    inject queue and was only absorbed at the top of a scheduling round.
    The polling body keeps yielding through the worker's FIFO ready
    queue, so the pop-miss drain never ran and each ask waited for up to
    MAX_ITER_PER_ROUND body turns: ~60 ms Soft / ~300 ms production.
  * At teardown the bench stopped the pool before cancelling the live
    body, so the production reclaimed must-wait (#3012/#3595) burned its
    full drain_ms x 8 budget (~16 s) for a fiber that could never run.

This test bounds the whole bench run with a 30 s timeout (the #4831 KILL)
and requires all 100 asks to answer. It also covers the explicit
--worker-threads=N opt-in so the multi-worker path stays fast.
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

# The issue's repro (#4831 shutdown/exit shape): one polling worker, 100 asks.
PROGRAM = """(define (hg h k d) (if (hash-has-key? h k) (hash-ref h k) d))
(orch:spawn-agent \"live4877\" (lambda ()
  (let loop ((n 0))
    (let ((m (orch:agent-recv \"live4877\" :wait #f)))
      (if (eq? #t (hg m \"ok\" #f))
          (begin (orch:agent-reply (hg m \"correlation-id\" 0) \"x\") (loop (+ n 1)))
          (begin (fiber:yield) (loop n)))))) :attach-mailbox #t)
(define ok-live4877 0)
(define t0 (monotonic-ms))
(let loop ((i 0))
  (if (< i 100)
      (begin
        (if (eq? #t (hg (orch:agent-ask \"live4877\" (number->string i) :timeout-ms 2000) \"ok\" #f))
            (set! ok-live4877 (+ ok-live4877 1)))
        (loop (+ i 1)))))
(display \"elapsed-ms=\") (display (- (monotonic-ms) t0)) (newline)
(if (= ok-live4877 100)
    (display \"OK-4831-shutdown\\n\")
    (begin (display \"FAIL-4831-shutdown asks=\") (display ok-live4877) (newline) (exit 1)))
"""


def _bench_env(soft: bool) -> dict[str, str]:
    env = dict(os.environ)
    env.pop("AURA_PIPELINE_STRICT", None)
    if soft:
        env["AURA_SANDBOX"] = "off"
    else:
        env.pop("AURA_SANDBOX", None)
    return env


class TestIssue4877(unittest.TestCase):
    def _run_once(self, label: str, soft: bool, extra_args: list[str]) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "f.aura"
            path.write_text(PROGRAM, encoding="utf-8")
            try:
                bench = subprocess.run(
                    [str(AURA_BIN), "--serve-async-bench", str(path), *extra_args],
                    capture_output=True,
                    text=True,
                    env=_bench_env(soft),
                    timeout=30,  # the #4831 bench KILL
                )
            except subprocess.TimeoutExpired:
                self.fail(f"{label}: bench did not finish inside the 30 s KILL")
            self.assertEqual(
                0,
                bench.returncode,
                f"{label}: bench failed\n{bench.stdout[-800:]}\n{bench.stderr[-800:]}",
            )
            self.assertIn(
                "OK-4831-shutdown",
                bench.stdout,
                f"{label}: not all 100 asks answered\n{bench.stdout[-800:]}",
            )
            self.assertNotIn("FAIL-4831-shutdown", bench.stdout, label)
            m = re.search(r"elapsed-ms=(\d+)", bench.stdout)
            self.assertIsNotNone(m, f"{label}: missing elapsed-ms\n{bench.stdout[-500:]}")
            # Pre-#4877 measured 3028 ms (Soft) / 9731 ms (production) just
            # for the asks; the fixed path is < 50 ms. Keep a wide guard so
            # a loaded CI box does not flake, while a per-ask regression
            # (60 ms / 300 ms) still trips it.
            self.assertLess(int(m.group(1)), 5000, f"{label}: asks regressed")

    def test_bench_100_asks_soft(self) -> None:
        self._run_once("soft", soft=True, extra_args=[])

    def test_bench_100_asks_production_default(self) -> None:
        self._run_once("production", soft=False, extra_args=[])

    def test_bench_100_asks_explicit_multi_worker(self) -> None:
        self._run_once("soft/multi-worker", soft=True, extra_args=["--worker-threads=2"])


if __name__ == "__main__":
    unittest.main()
