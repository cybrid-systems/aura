#!/usr/bin/env python3
r"""Regression for #5056: soft REPL no longer reports false type warnings.

Three separate defects, all visible only through the REPL's soft face:

  * (if (and …)) / (if (or …)) reported "call return type mismatch:
    expected Bool, got Any".  and/or yield their last value, not a Bool,
    and an if condition is a truthiness position (only #f is false).
  * (mutate:rebind name (format …) summary) reported "argument 1:
    expected String, got Any" — format had no checker signature and
    mutate:rebind's signature stopped at two parameters.
  * Every warning printed twice: check_before_lowering runs infer_flat
    and then the pure check over the same tree into one collector.

The test drives the real CLI, so it covers the same surface the issue
was filed from.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def run_repl(lines: list[str]) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(k, None)
    return subprocess.run(
        [str(AURA_BIN), "repl"],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
    )


def eval_expr(expr: str) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT"):
        env.pop(k, None)
    return subprocess.run(
        [str(AURA_BIN), "--preset", "dev", "-e", expr],
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
        stdin=subprocess.DEVNULL,
    )


class SoftReplTypeWarnings(unittest.TestCase):
    def test_if_and_or_conditions_are_quiet(self) -> None:
        r = run_repl(
            [
                "(define (f) (if (and #t #t) 1 0))",
                "(define (g) (if (or #f #t) 1 0))",
                "(define (h) (if (not #f) 1 0))",
                "(define (k) (if (eq? 1 1) 1 0))",
            ]
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("type warning", r.stderr, r.stderr)
        for name in ("f", "g", "h", "k"):
            self.assertIn(f'"wrote" ("{name}")', r.stdout, r.stdout)

    def test_format_into_rebind_is_quiet(self) -> None:
        r = run_repl(
            [
                '(set-code "(define (guess) 0)")',
                "(eval-current)",
                '(define (h) (mutate:rebind "guess" (format "(define (guess) ~a)" 7) "evolve"))',
            ]
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("type warning", r.stderr, r.stderr)

    def test_dynamic_argument_is_a_checked_boundary(self) -> None:
        # A Dynamic value (here: the result of an unsigned helper) passed to a
        # statically-typed parameter is a gradual boundary. It must not be
        # reported as a type error; the deferred CoercionNode does the runtime
        # check. This is the evolve! shape from the issue thread.
        r = run_repl(
            [
                '(set-code "(define (params) (list 0 0))")',
                "(eval-current)",
                '(define (params->src c) (format "(define (params) ~a)" c))',
                '(define (evolve! c) (mutate:rebind "params" (params->src c) "evolve"))',
            ]
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("type warning", r.stderr, r.stderr)

    def test_a_real_mismatch_prints_once(self) -> None:
        r = run_repl(["(define (bad) (string-length 1))"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(
            r.stderr.count("type warning (bad)"),
            1,
            "one diagnostic must be printed once, not once per check pass:\n" + r.stderr,
        )
        self.assertIn("expected String, got Int", r.stderr)

    def test_runtime_truthiness_unchanged(self) -> None:
        # and/or still return their last value; if only treats #f as false.
        r = eval_expr("(display (and 1 2))")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip(), "2")
        r = eval_expr("(display (or #f 3))")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip(), "3")
        r = eval_expr("(display (if (and #t #t) 1 0))")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip(), "1")
        r = eval_expr('(display (if (and #f "x") 1 0))')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip(), "0")


if __name__ == "__main__":
    unittest.main()
