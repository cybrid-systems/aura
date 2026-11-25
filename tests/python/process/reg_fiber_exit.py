#!/usr/bin/env python3
"""Issue #5436: a fiber body still running when the program ends.

The evaluator drain gives up on a body after two seconds and detaches it.
The teardown then freed the heaps under it and the process crashed
("double free or corruption", SIGSEGV in std::vector<EvalValue>::push_back)
instead of exiting with the status the run had decided on.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))
from _aura_harness import AURA_BIN  # noqa: E402

CRASH_MARKERS = ("AURA CRASH", "double free", "corrupted", "Segmentation fault")

# A body with no safepoint: it outlives the drain's two seconds for sure.
STUCK_BODY = "(define spin (fiber:spawn (lambda () (let loop ((i 0)) (if (< i 40000000) (loop (+ i 1)) 'done)))))"

WEBHOOK_PROGRAM = textwrap.dedent(
    """\
    (require "std/webhook" all:)
    (require "std/socket" all:)
    ; Issue #5436: the handler is still computing when the program stops the
    ; server and ends. stop! must say so, and the process must still exit
    ; with its own status.
    (define s (webhook:make (hash "port" 0 "drain-ms" 200 "cancel-ms" 200)))
    (webhook:route! s "/slow" (list "GET")
      (lambda (req) (let loop ((i 0)) (if (< i 40000000) (loop (+ i 1)) "done"))))
    (webhook:start! s)
    (define port (webhook:port s))
    (define client
      (fiber:spawn (lambda ()
        (let ((c (tcp-connect "127.0.0.1" port)))
          (tcp-send c "GET /slow HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n")
          (let loop ((seen 0))
            (let ((chunk (tcp-recv c 4096)))
              (if (string? chunk) (loop (+ seen (string-length chunk))) seen)))))))
    (let loop ()
      (if (= (hash-ref (webhook:stats s) "inflight") 0) (begin (fiber:sleep 5) (loop)) #f))
    (display "stop! -> ")
    (display (try (webhook:stop! s) (catch (e) (list 'refused (format "~a" e)))))
    (newline)
    """
)


class StuckBodyExit(unittest.TestCase):
    def run_program(self, program: str, *argv: str) -> subprocess.CompletedProcess[str]:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "probe.aura"
            path.write_text(textwrap.dedent(program))
            env = dict(os.environ)
            env["AURA_SANDBOX"] = "off"
            env.pop("AURA_PIPELINE_STRICT", None)
            try:
                return subprocess.run(
                    [str(AURA_BIN), *argv, str(path)],
                    capture_output=True,
                    text=True,
                    timeout=60,
                    env=env,
                )
            except subprocess.TimeoutExpired:
                self.fail("aura did not exit with a fiber body still running")

    def assert_no_crash(self, run: subprocess.CompletedProcess[str]) -> None:
        for stream in (run.stdout, run.stderr):
            for marker in CRASH_MARKERS:
                self.assertNotIn(marker, stream)
        self.assertNotIn(run.returncode, (134, 139), f"crashed: {run.stderr[-400:]}")

    def test_program_end_exits_with_its_status(self) -> None:
        run = self.run_program(f'{STUCK_BODY}\n(fiber:sleep 200)\n(display "program done") (newline)\n', "--load")
        self.assert_no_crash(run)
        self.assertEqual(run.returncode, 0, run.stderr[-400:])
        self.assertIn("program done", run.stdout)
        # the body was really still running: the drain had to abandon it
        self.assertIn("[fiber:drain] WARN", run.stderr)

    def test_exit_keeps_its_code(self) -> None:
        run = self.run_program(f"{STUCK_BODY}\n(fiber:sleep 200)\n(exit 3)\n", "--load")
        self.assert_no_crash(run)
        self.assertEqual(run.returncode, 3, run.stderr[-400:])
        self.assertIn("[fiber:drain] WARN", run.stderr)

    def test_uncaught_error_keeps_its_status(self) -> None:
        run = self.run_program(f'{STUCK_BODY}\n(fiber:sleep 200)\n(error "boom")\n', "--load")
        self.assert_no_crash(run)
        self.assertEqual(run.returncode, 1, run.stderr[-400:])
        self.assertIn("boom", run.stderr)

    def test_six_stuck_bodies_share_one_drain_budget(self) -> None:
        # Issue #5463: each abandoned body used to wait its own 2s, so six
        # stuck bodies took ~12s. The drain has one shared absolute deadline.
        program = """\
(define (stuck _) (fiber:spawn (lambda () (let loop () (loop)))))
(for-each stuck '(1 2 3 4 5 6))
(fiber:sleep 200)
(exit 3)
"""
        started = time.monotonic()
        run = self.run_program(program, "--load")
        elapsed = time.monotonic() - started
        self.assert_no_crash(run)
        self.assertEqual(run.returncode, 3, run.stderr[-400:])
        self.assertEqual(run.stderr.count("[fiber:drain] WARN"), 6, run.stderr[-800:])
        self.assertLess(elapsed, 6.0, f"six stuck fibers took {elapsed:.1f}s")

    def test_webhook_handler_outliving_stop(self) -> None:
        run = self.run_program(WEBHOOK_PROGRAM)
        self.assert_no_crash(run)
        self.assertEqual(run.returncode, 0, run.stderr[-400:])
        self.assertIn("stop! -> (refused webhook:stop!: 1 handler(s) still running", run.stdout)
