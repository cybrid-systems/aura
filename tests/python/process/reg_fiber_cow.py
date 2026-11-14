#!/usr/bin/env python3
"""Regression tests for issues #4838-#4846 (fiber join/cancel, COW, macros, bench).

#4838 fiber inline lambda try/catch keeps the raised payload.
#4839 a second fiber:join is idempotent instead of hanging / returning ().
#4840 cancel of a fiber parked in channel:recv lets the following join return.
#4841 (covered separately: cancellation memory safety).
#4844 --serve-async-bench runs fiber lambdas under the default policy and
       returns a nonzero exit code when a form fails.
#4845 overlay-set!/overlay-commit without an open overlay are rejected.
#4846 defmacro works under the default (production) pipeline policy.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def _env() -> dict[str, str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    # Exercise the default policy: a stale inherited override would hide #4844/#4846.
    env.pop("AURA_PIPELINE_STRICT", None)
    return env


def _run(source: str, *args: str, timeout: float = 30.0) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    try:
        return subprocess.run(
            [str(AURA_BIN), *args, path],
            capture_output=True,
            text=True,
            env=_env(),
            timeout=timeout,
        )
    finally:
        os.unlink(path)


class TestIssueBatch4838To4846(unittest.TestCase):
    def test_4838_fiber_inline_lambda_keeps_payload(self) -> None:
        proc = _run(
            """
(display (list 'fiber-inline (fiber:join (fiber:spawn (lambda () (try (car 5) (catch (e) e))))))) (newline)
(display (list 'fiber-inline-raise (fiber:join (fiber:spawn (lambda () (try (raise 'oops) (catch (e) e))))))) (newline)
(display (list 'fiber-inline-error (fiber:join (fiber:spawn (lambda () (try (error "boom") (catch (e) e))))))) (newline)
""",
            "--load",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("(fiber-inline car: not a pair)", proc.stdout)
        self.assertIn("(fiber-inline-raise oops)", proc.stdout)
        self.assertIn("(fiber-inline-error boom)", proc.stdout)
        self.assertNotIn("type error:", proc.stdout)

    def test_4839_second_join_returns_same_value(self) -> None:
        proc = _run(
            """
(define f (fiber:spawn (lambda () 7)))
(display (list 'first (fiber:join f))) (newline)
(display (list 'second (fiber:join f))) (newline)
""",
            "--load",
            timeout=20.0,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("(first 7)", proc.stdout)
        self.assertIn("(second 7)", proc.stdout)

    def test_4840_cancel_parked_recv_then_join(self) -> None:
        proc = _run(
            """
(define ch (channel:create 4))
(define f (fiber:spawn (lambda () (channel:recv ch))))
(fiber:yield)
(display (list 'cancel (fiber:cancel f))) (newline)
(display (list 'join (try (fiber:join f) (catch (e) (list 'err e))))) (newline)
""",
            "--load",
            timeout=20.0,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("(cancel #t)", proc.stdout)
        self.assertIn("fiber-cancelled", proc.stdout)

    def test_4845_overlay_misuse_is_not_silent(self) -> None:
        proc = _run(
            """
(display (list 'set-no-tx (try (mutate :overlay-set! "k" 5) (catch (e) (list 'err e))))) (newline)
(display (list 'commit-no-tx (mutate :overlay-commit))) (newline)
(display (list 'rollback-no-tx (mutate :overlay-rollback))) (newline)
""",
            "--load",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("no open overlay", proc.stdout)
        self.assertIn("(commit-no-tx #f)", proc.stdout)
        self.assertIn("(rollback-no-tx #f)", proc.stdout)

    def test_4846_defmacro_works_under_default_policy(self) -> None:
        proc = _run(
            """
(defmacro (sw a b) (list (quote list) b a))
(display (sw 1 2))
""",
            "--load",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), "(2 1)")

    def test_4844_bench_fiber_default_policy_and_exit_code(self) -> None:
        ok = _run(
            """
(define f (fiber:spawn (lambda () 42)))
(display (fiber:join f)) (newline)
""",
            "--serve-async-bench",
            timeout=30.0,
        )
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertIn("42", ok.stdout)

        bad = _run("(car 5)\n", "--serve-async-bench", timeout=30.0)
        self.assertNotEqual(bad.returncode, 0, "eval failure must not exit 0")
        self.assertIn("eval error", bad.stderr)


class TestIssue4848DefaultPipelineForms(unittest.TestCase):
    """#4848: the default policy must run ordinary language forms."""

    def _assert_program(self, source: str, expected: list[str]) -> None:
        proc = _run(source, "--load")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        for line in expected:
            self.assertIn(line, proc.stdout)

    def test_4848_let_and_lambda_args(self) -> None:
        self._assert_program(
            """
(display (let ((a 1) (b 2)) (+ a b))) (newline)
(display (map (lambda (x) (* x 2)) (list 1 2 3))) (newline)
(display (let loop ((i 0)) (if (< i 3) (loop (+ i 1)) i))) (newline)
""",
            ["3", "(2 4 6)"],
        )

    def test_4848_set_do_try_and_internal_define(self) -> None:
        self._assert_program(
            """
(define x 0) (set! x 5) (display x) (newline)
(display (do ((i 0 (+ i 1))) ((= i 3) i))) (newline)
(display (try (raise 'x) (catch (e) e))) (newline)
(define (f n) (define (g x) (* x 2)) (g n)) (display (f 4)) (newline)
""",
            ["5", "3", "x", "8"],
        )

    def test_4848_fiber_lambda_under_load(self) -> None:
        self._assert_program(
            """
(display (fiber:join (fiber:spawn (lambda () 5)))) (newline)
(define (body) 5) (display (fiber:join (fiber:spawn body))) (newline)
""",
            ["5"],
        )

    def test_4848_operator_override_still_wins(self) -> None:
        proc = _run("(display (let ((a 1)) a)) (newline)", "--load")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "1"
        with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
            f.write('(display (set-code "(define (f x) x)")) (newline)')
            path = f.name
        try:
            strict = subprocess.run(
                [str(AURA_BIN), "--load", path], capture_output=True, text=True, env=env, timeout=30.0
            )
        finally:
            os.unlink(path)
        self.assertEqual(strict.returncode, 1)
        self.assertIn("tree-walker fallback forbidden", strict.stderr)


class TestIssue4855TypeErrorNamesOperation(unittest.TestCase):
    """#4855: a type error names the operation and the failing argument."""

    def test_4855_arith_error_names_op_and_argument(self) -> None:
        proc = _run(
            """
(define (add-cost total item)
  (+ total item))
(add-cost 1 "two")
""",
            "--load",
        )
        self.assertEqual(proc.returncode, 1)
        self.assertIn("type mismatch", proc.stderr)
        self.assertIn("in +, argument 2", proc.stderr)
        self.assertIn("got String 'two'", proc.stderr)


if __name__ == "__main__":
    unittest.main()
