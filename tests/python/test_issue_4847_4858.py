#!/usr/bin/env python3
"""Regression tests for issues #4847-#4858 (orch/COW/pipeline batch).

#4847 an orch agent that joined cleanly must not crash the process at exit
      (the pre-fix SIGSEGV in Fiber::~Fiber made every rc 139).
#4852 fiber:overlay-set! on an unbound name reports the binding name and the
      overlay-define hint instead of the literal " + name + ".
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


def _env() -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    # Exercise the default policy: a stale inherited override would hide #4848.
    env.pop("AURA_PIPELINE_STRICT", None)
    return env


def _run(
    source: str, *args: str, timeout: float = 30.0, pipeline_strict: str | None = None
) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = _env()
    if pipeline_strict is not None:
        env["AURA_PIPELINE_STRICT"] = pipeline_strict
    try:
        return subprocess.run(
            [str(AURA_BIN), *args, path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
    finally:
        os.unlink(path)


class TestIssueBatch4847To4858(unittest.TestCase):
    def test_4847_agent_join_exits_clean(self) -> None:
        # Issue #4847 was 10/10 SIGSEGV at exit with the default pipeline
        # face and clean under force-soa. Five runs per face pin both sides
        # of the regression (ten process exits total) and keep a single
        # stray teardown crash from hiding behind a green first run.
        source = """
(orch:spawn-agent "w" (lambda () 1))
(display (hash-ref (orch:agent-join "w" :timeout-ms 3000) "status")) (newline)
"""
        for strict in ("0", "force-soa"):
            for run in range(5):
                with self.subTest(pipeline_strict=strict, run=run):
                    proc = _run(
                        source,
                        "--load",
                        timeout=60.0,
                        pipeline_strict=strict,
                    )
                    self.assertEqual(proc.returncode, 0, f"run {run}/{strict}: {proc.stderr}")
                    self.assertIn("ok", proc.stdout)
                    self.assertNotIn("SIGSEGV", proc.stderr)
                    self.assertNotIn("double free", proc.stderr)

    def test_4852_overlay_set_unbound_names_binding(self) -> None:
        proc = _run(
            """
(fiber:overlay-begin)
(display (try (fiber:overlay-set! "never-defined-key" 5)
              (catch (e) (list 'err (format "~a" e))))) (newline)
(fiber:overlay-rollback)
""",
            "--load",
            pipeline_strict="0",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("unbound binding never-defined-key", proc.stdout)
        self.assertIn("fiber:overlay-define", proc.stdout)
        self.assertNotIn(" + name + ", proc.stdout)

    def test_4850_agent_join_keeps_raised_symbol(self) -> None:
        proc = _run(
            """
(orch:spawn-agent "s" (lambda () (raise 'rate-limited)))
(display (hash-ref (orch:agent-join "s" :timeout-ms 3000) "error")) (newline)
""",
            "--load",
            timeout=60.0,
            pipeline_strict="0",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("rate-limited", proc.stdout)
        self.assertNotIn("runtime error", proc.stdout)

    def test_4849_overlay_pending_writes_are_gc_roots(self) -> None:
        proc = _run(
            """
(define (range n) (let loop ((i (- n 1)) (acc (list))) (if (< i 0) acc (loop (- i 1) (cons i acc)))))
(define (churn n) (let loop ((i 0) (acc (list))) (if (< i n) (loop (+ i 1) (if (= 0 (modulo i 100)) (list) (cons (list i i) acc))) 'done)))
(fiber:overlay-begin) (fiber:overlay-define "a" (range 10)) (churn 200000) (fiber:overlay-commit)
(display "define pending+churn: ") (write (fiber:overlay-get "a" 'none)) (newline)
(fiber:overlay-begin) (fiber:overlay-define "b" 0) (fiber:overlay-commit)
(fiber:overlay-begin) (fiber:overlay-set! "b" (range 10)) (churn 200000)
(display "set! pending, own view: ") (write (fiber:overlay-get "b" 'none)) (newline)
(fiber:overlay-commit)
(display "set! pending+churn committed: ") (write (fiber:overlay-get "b" 'none)) (newline)
""",
            "--load",
            timeout=180.0,
            pipeline_strict="0",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        want = "(0 1 2 3 4 5 6 7 8 9)"
        self.assertIn("define pending+churn: " + want, proc.stdout)
        self.assertIn("set! pending, own view: " + want, proc.stdout)
        self.assertIn("set! pending+churn committed: " + want, proc.stdout)

    def test_4858_restart_policy_replays_failed_scope_child(self) -> None:
        proc = _run(
            """
(define attempts (hash))
(hash-set! attempts "n" 0)
(orch:scope-spawn "flaky" (lambda () (hash-set! attempts "n" (+ 1 (hash-ref attempts "n")))
  (if (< (hash-ref attempts "n") 3) (error "flaky") 'finally)))
(define r (orch:scope-join-all :timeout-ms 5000 :on-join-fail "restart" :max-restarts 3))
(display (list (hash-ref attempts "n") (hash-ref r "restart-attempted")
               (hash-ref r "restart-ok") (hash-ref r "restart-exhausted"))) (newline)
""",
            "--load",
            timeout=60.0,
            pipeline_strict="0",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        # The child failed twice; RestartN re-spawned it both times and the
        # third attempt succeeded. No silent no-op: the counter is visible.
        self.assertIn("(3 2 2 0)", proc.stdout)

    def test_4851_unknown_join_fail_policy_is_an_error(self) -> None:
        proc = _run(
            """
(orch:scope-spawn "t" (lambda () 1))
(display (orch:scope-join-all :timeout-ms 2000 :on-join-fail "cancle")) (newline)
""",
            "--load",
            timeout=30.0,
            pipeline_strict="0",
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("unknown :on-join-fail", proc.stderr)
        self.assertIn("cancel / restart-n / throttle / report-only", proc.stderr)

    def test_4851_cancel_acts_on_the_first_body_failure(self) -> None:
        import time

        started = time.monotonic()
        proc = _run(
            """
(define (spin-ms ms) (let ((t (+ (monotonic-ms) ms))) (let loop ()
  (when (< (monotonic-ms) t) (fiber:yield) (loop)))))
(orch:scope-spawn "bad" (lambda () (spin-ms 20) (error "bad sibling")))
(orch:scope-spawn "slow0" (lambda () (spin-ms 4000) 'late))
(orch:scope-spawn "slow1" (lambda () (spin-ms 4000) 'late))
(orch:scope-spawn "slow2" (lambda () (spin-ms 4000) 'late))
(orch:scope-spawn "slow3" (lambda () (spin-ms 4000) 'late))
(define r (orch:scope-join-all :timeout-ms 6000 :on-join-fail "cancel"))
(display (list (hash-ref r "status") (hash-ref r "err-count")
               (hash-ref r "cancelled-count"))) (newline)
""",
            "--load",
            timeout=30.0,
            pipeline_strict="0",
        )
        elapsed = time.monotonic() - started
        self.assertEqual(proc.returncode, 0, proc.stderr)
        # Issue #4851: the failing sibling is spawned FIRST, so the scope
        # body queue has to start it in spawn order (the old LIFO deque ran
        # it LAST, behind four 4 s siblings, so the join waited the whole
        # :timeout-ms and reported timeout / err-count 0). With spawn-order
        # dispatch the failing body runs after ~20 ms and :on-join-fail
        # "cancel" cancels the four siblings that have not started yet.
        # Accept error or partial as the aggregate status; both mean the
        # failing body was counted and the rest cancelled.
        # The cancelled count is 3 or 4 depending on whether the last slow
        # body had already started when fail-fast fired; the failing body and
        # the fast return are the invariants.
        self.assertRegex(proc.stdout, r"\((partial|error) 1 [34]\)")
        self.assertLess(elapsed, 5.5, proc.stdout)

    def test_4851_scope_bodies_start_in_spawn_order(self) -> None:
        # Issue #4851: scope bodies are dispatched on the worker's FIFO
        # start-order queue, so they BEGIN in spawn order (the LIFO deque
        # started the last-spawned body first, which hid a failing first
        # sibling behind the slow ones).
        proc = _run(
            """
(define order (list))
(define (note n) (set! order (append order (list n))))
(orch:scope-spawn "a" (lambda () (note "a")))
(orch:scope-spawn "b" (lambda () (note "b")))
(orch:scope-spawn "c" (lambda () (note "c")))
(orch:scope-join-all :timeout-ms 5000)
(display order) (newline)
""",
            "--load",
            timeout=30.0,
            pipeline_strict="0",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("(a b c)", proc.stdout)


if __name__ == "__main__":
    unittest.main()
