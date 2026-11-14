#!/usr/bin/env python3
"""Regression tests for issue #4855 (diagnostic quality).

#4855 asks for three things on top of the already-landed op name:

  * the caught message names the operation AND the failing argument,
  * the UNCAUGHT error is reported ONCE and its position points at the
    INNER failing form (not the outermost call that swallowed it),
  * (hash-ref h key) with a missing key and no default RAISES an error
    that names the key; (hash-ref h key default) keeps returning the
    default.
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
    # The permissive pipeline face: the strict face vetoes the path with an
    # internal error instead of the real symptom (issue #4848).
    env["AURA_PIPELINE_STRICT"] = "0"
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


# The repro from the issue body. The inner (+ total item) sits at 2:3 and
# the outermost call at 6:1.
ISSUE_REPRO = """
(define (add-cost total item)
  (+ total item))
(display (try (add-cost 1 "two") (catch (e) (list 'caught (format "~a" e))))) (newline)
(define cfg (hash "model" "glm"))
(display (list 'missing (hash-ref cfg "modle"))) (newline)
(add-cost 1 "two")
"""


class TestIssue4855DiagnosticQuality(unittest.TestCase):
    """#4855: an agent-facing type error names the op, the argument and the
    failing form; hash-ref does not swallow a missing key."""

    def test_4855_caught_message_names_operation_and_argument(self) -> None:
        proc = _run(
            '(display (try (+ 1 "two") (catch (e) (format "~a" e)))) (newline)\n',
            "--load",
        )
        self.assertIn("type mismatch", proc.stdout, proc.stderr)
        self.assertIn("in +, argument 2", proc.stdout, proc.stderr)
        self.assertIn("got String", proc.stdout, proc.stderr)

    def test_4855_uncaught_error_is_reported_once_at_the_inner_form(self) -> None:
        proc = _run(
            '(define (add-cost total item)\n  (+ total item))\n(add-cost 1 "two")\n',
            "--load",
        )
        self.assertEqual(proc.returncode, 1, proc.stderr)
        # ONE report of the error (not the plain line AND the located one).
        self.assertEqual(proc.stderr.count("type mismatch"), 1, proc.stderr)
        reported = [ln for ln in proc.stderr.splitlines() if ln.startswith("error:")]
        self.assertEqual(len(reported), 1, proc.stderr)
        # The position is the INNER failing form (2:3), not the outermost
        # call (3:1) that the error escaped through. The parser records a
        # call at its head symbol, so the column is the operator's, not the
        # open paren's -- the LINE is what matters here.
        self.assertRegex(proc.stderr, r"\b2:\d+: type error:")
        self.assertNotIn("3:1", proc.stderr)
        # The excerpt shows the inner failing form.
        self.assertIn("(+ total item)", proc.stderr)
        self.assertIn("in +, argument 2", proc.stderr)

    def test_4855_issue_repro_hash_ref_raises_on_missing_key(self) -> None:
        proc = _run(ISSUE_REPRO, "--load")
        # The caught type error names the operation and the argument.
        self.assertIn("in +, argument 2", proc.stdout, proc.stderr)
        # hash-ref with a missing key and no default raises instead of
        # silently returning ().
        self.assertIn("hash-ref", proc.stderr)
        self.assertIn("modle", proc.stderr)
        self.assertNotIn("not found)", proc.stdout)

    def test_4855_hash_ref_default_still_returns_the_default(self) -> None:
        proc = _run(
            '(define cfg (hash "model" "glm"))\n'
            '(display (hash-ref cfg "modle" "fallback")) (newline)\n'
            '(display (hash-ref cfg "model" "fallback")) (newline)\n',
            "--load",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("fallback", proc.stdout)
        self.assertIn("glm", proc.stdout)
        self.assertNotIn("hash-ref", proc.stderr)

    def test_4855_hash_ref_missing_key_is_catchable(self) -> None:
        proc = _run(
            '(define cfg (hash "model" "glm"))\n'
            '(display (try (hash-ref cfg "modle") (catch (e) (format "~a" e)))) (newline)\n',
            "--load",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("hash-ref: key", proc.stdout, proc.stderr)
        self.assertIn("modle", proc.stdout, proc.stderr)

    def test_4855_ir_executor_comparison_names_the_operation(self) -> None:
        # The IR executor (--ir) is the other face that must name the op.
        proc = subprocess.run(
            [str(AURA_BIN), "--ir", '(define (cmp a b) (< a b)) (cmp 1 "two")'],
            capture_output=True,
            text=True,
            env=_env(),
            timeout=30.0,
        )
        self.assertIn("in <, argument 2", proc.stderr)


if __name__ == "__main__":
    unittest.main()
