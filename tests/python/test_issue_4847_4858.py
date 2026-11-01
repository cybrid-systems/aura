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
        proc = _run(
            """
(orch:spawn-agent "w" (lambda () 1))
(display (hash-ref (orch:agent-join "w" :timeout-ms 3000) "status")) (newline)
""",
            "--load",
            timeout=60.0,
            pipeline_strict="0",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("ok", proc.stdout)
        self.assertNotIn("SIGSEGV", proc.stderr)

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


if __name__ == "__main__":
    unittest.main()
