#!/usr/bin/env python3
"""Regression for #4873: a blocking in-body orch:agent-recv must not starve peers.

Two :attach-mailbox agents whose bodies block in (orch:agent-recv :wait #t)
used to occupy the scheduler worker with a 1 ms sleep loop, so the sibling
agent's body did not start until the recv timeout expired (and an
orch:agent-ask to it timed out). The recv wait now parks the fiber with a
short deadline inside the soft orch agent-body window, handing the worker to
sibling agents.
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AURA_BIN = ROOT / "build" / "aura"

RECV_TIMEOUT_MS = 1500
START_BOUND_MS = 1000  # well before the 1500 ms recv timeout

# One blocking worker ("b") and one trivial sender ("s"). The spawn order is
# the parameter: the starvation bug depended on which agent the scheduler
# started first.
REPRO_SPAWN_ORDER = r"""
(define (hg h k d) (if (hash-has-key? h k) (hash-ref h k) d))
(define cb (channel:create 8))
(define cs (channel:create 8))
(define t0 (monotonic-ms))
(define (blocking-body)
  (channel:send cb (list 'b-start (- (monotonic-ms) t0)))
  (let ((m (orch:agent-recv "b" :wait #t :timeout-ms TIMEOUT)))
    (channel:send cb (list 'b-recv-returned (- (monotonic-ms) t0) (hg m "ok" '?)))
    'bdone))
(define (sender-body)
  (channel:send cs (list 's-start (- (monotonic-ms) t0)))
  'sdone)
FIRST
(orch:agent-join "s" :timeout-ms 5000)
(orch:agent-join "b" :timeout-ms 5000)
(display (list (channel:try-recv cb) (channel:try-recv cb) (channel:try-recv cs)))
(newline)
"""

REPRO_TWO_BLOCKING = r"""
(define (hg h k d) (if (hash-has-key? h k) (hash-ref h k) d))
(define t0 (monotonic-ms))
(define (blocker name)
  (lambda ()
    (let loop ()
      (let ((m (orch:agent-recv name :wait #t :timeout-ms 3000)))
        (if (eq? #t (hg m "ok" #f))
            (begin
              (orch:agent-reply (hg m "correlation-id" 0)
                                (string-append "re:" (hg m "payload" "")))
              (loop))
            'done)))))
FIRST_SPAWN
SECOND_SPAWN
(define a-first (orch:agent-ask "FIRST_NAME" "to-first" :timeout-ms 1500))
(define a-second (orch:agent-ask "SECOND_NAME" "to-second" :timeout-ms 1500))
(display (list 'ask-first (hg a-first "ok" '?)
               'ask-second (hg a-second "ok" '?)
               'ms (- (monotonic-ms) t0)))
(newline)
"""


def _env(sandbox_off: bool) -> dict[str, str]:
    env = dict(os.environ)
    if sandbox_off:
        env["AURA_SANDBOX"] = "off"
    else:
        env.pop("AURA_SANDBOX", None)
    env["AURA_PIPELINE_STRICT"] = "0"
    return env


def _run(source: str, *, mode: str, sandbox_off: bool, timeout: float = 20.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    try:
        argv = [str(AURA_BIN)]
        if mode == "load":
            argv.append("--load")
        argv.append(path)
        return subprocess.run(
            argv,
            capture_output=True,
            text=True,
            env=_env(sandbox_off),
            timeout=timeout,
        )
    finally:
        os.unlink(path)


class TestBlockingRecvYields4873(unittest.TestCase):
    def _modes(self):
        for mode in ("load", "direct"):
            for sandbox_off in (False, True):
                yield mode, sandbox_off

    def test_second_agent_starts_before_recv_timeout(self) -> None:
        first_blocking = (
            '  (orch:spawn-agent "b" (lambda () (blocking-body))'
            " :attach-mailbox #t :region-key 1)\n"
            '  (orch:spawn-agent "s" (lambda () (sender-body)) :region-key 2)\n'
        )
        first_sender = (
            '  (orch:spawn-agent "s" (lambda () (sender-body)) :region-key 2)\n'
            '  (orch:spawn-agent "b" (lambda () (blocking-body))'
            " :attach-mailbox #t :region-key 1)\n"
        )
        for label, spawn in (("blocking-first", first_blocking), ("blocking-second", first_sender)):
            source = REPRO_SPAWN_ORDER.replace("TIMEOUT", str(RECV_TIMEOUT_MS)).replace("FIRST", spawn)
            for mode, sandbox_off in self._modes():
                with self.subTest(label=label, mode=mode, sandbox_off=sandbox_off):
                    p = _run(source, mode=mode, sandbox_off=sandbox_off)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    out = p.stdout
                    start = re.search(r"\(s-start (\d+)\)", out)
                    self.assertIsNotNone(start, out)
                    recv = re.search(r"\(b-recv-returned (\d+) #f\)", out)
                    self.assertIsNotNone(recv, out)
                    s_start = int(start.group(1))
                    b_returned = int(recv.group(1))
                    self.assertLess(
                        s_start,
                        START_BOUND_MS,
                        f"second agent started after {s_start} ms ({label}/{mode}/"
                        f"sandbox_off={sandbox_off}); output={out}",
                    )
                    # The blocking recv still honoured its wait budget.
                    self.assertGreaterEqual(b_returned, 1000, out)

    def test_ask_to_second_blocking_worker_succeeds(self) -> None:
        def spawn(name: str) -> str:
            region = 1 if name == "w1" else 2
            return f'  (orch:spawn-agent "{name}" (blocker "{name}") :attach-mailbox #t :region-key {region})\n'

        for label, first, second in (
            ("w1-then-w2", "w1", "w2"),
            ("w2-then-w1", "w2", "w1"),
        ):
            source = (
                REPRO_TWO_BLOCKING.replace("FIRST_SPAWN", spawn(first))
                .replace("SECOND_SPAWN", spawn(second))
                .replace("FIRST_NAME", first)
                .replace("SECOND_NAME", second)
            )
            for mode, sandbox_off in self._modes():
                for i in range(3):
                    with self.subTest(label=label, mode=mode, sandbox_off=sandbox_off, run=i):
                        p = _run(source, mode=mode, sandbox_off=sandbox_off)
                        self.assertEqual(p.returncode, 0, p.stderr)
                        self.assertIn("ask-first #t", p.stdout)
                        self.assertIn("ask-second #t", p.stdout)


if __name__ == "__main__":
    unittest.main()
