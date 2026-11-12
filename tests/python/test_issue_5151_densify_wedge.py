#!/usr/bin/env python3
"""Regression for #5151.

Under the restricted sandbox (`aura soft` / `--preset dev`) one mutate of a
comparison-callee workspace armed the Moving densify throttle even though
nothing was relocated. Every later mutate came back
`AdmissionRejected: densify-throttle`, snapshot returned -1, and closure
apply died with `Moving densify window incomplete`.

The layout that wedges on this tree is the #4782 lambda form (a comparison
in the callee and a multi-character caller). The issue's define-sugar repro
is locked too. A refused snapshot (no live workspace) must not stick: the
next rebind, snapshot, and restore are admitted, and format of the fail
reason is the keyword name (`:no-workspace` / `:guard-reject` / `:none`),
never `<unknown>`.
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

# Issue repro: define-sugar, comparison callee, caller name length >= 2.
PROG_SUGAR = r"""
(set-code "(define (p x) (> x 10))\n(define (e1 x) (p x))")
(display (format "snapshot=~a\n" (mutate :snapshot "s0")))
(display (format "rebind=~a\n" (mutate :rebind "p" "(lambda (x) (> x 3))" "v")))
(display (format "call=~a\n" (map (lambda (x) x) (list 1))))
"""

# The shape that failed restore_forward_4782 under --preset dev.
PROG_LAMBDA = r"""
(set-code "(define p (lambda (x) (> x 10)))\n(define e4782 (lambda (x) (p x)))")
(eval-current)
(define s0 (mutate :snapshot "s0"))
(define rb (mutate :rebind "p" "(lambda (x) (> x 3))" "v2"))
(eval-current)
(define call (e4782 5))
(define mapped (map (lambda (x) x) (list 1)))
(display (format "s0=~a\n" s0))
(display (format "rb=~a\n" rb))
(display (format "call=~a\n" call))
(display (format "mapped=~a\n" mapped))
(display (format "reason=~a\n" (query :snapshot-fail-reason)))
"""

# A refused mutate, then a valid rebind + snapshot + restore.
PROG_RECOVER = r"""
(define bad (mutate :snapshot "nope"))
(display (format "bad=~a\n" bad))
(display (format "reason=~a\n" (query :snapshot-fail-reason)))
(display (format "kw=~a\n" :guard-reject))
(display (format "mapped=~a\n" (map (lambda (x) x) (list 1))))
(set-code "(define p (lambda (x) (> x 10)))\n(define e4782 (lambda (x) (p x)))")
(eval-current)
(define s0 (mutate :snapshot "s0"))
(define rb (mutate :rebind "p" "(lambda (x) (> x 3))" "v2"))
(eval-current)
(define call (e4782 5))
(define s1 (mutate :snapshot "s1"))
(define rest (mutate :restore s0))
(eval-current)
(define after (e4782 5))
(display (format "s0=~a\n" s0))
(display (format "rb=~a\n" rb))
(display (format "call=~a\n" call))
(display (format "s1=~a\n" s1))
(display (format "rest=~a\n" rest))
(display (format "after=~a\n" after))
(display (format "reason2=~a\n" (query :snapshot-fail-reason)))
"""

SESSIONS = {
    "soft": ["soft"],
    "preset-dev": ["--preset", "dev"],
}


def _env() -> dict[str, str]:
    env = dict(os.environ)
    for k in ("AURA_PIPELINE_STRICT", "AURA_SANDBOX", "AURA_ARENA_MOVING_COMPACT", "AURA_PRESET"):
        env.pop(k, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)
    return env


def _lines(stdout: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for raw in stdout.splitlines():
        line = raw.strip()
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        out[key] = val
    return out


class DensifyWedge5151(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not AURA_BIN.exists():
            raise unittest.SkipTest(f"aura binary not found: {AURA_BIN}")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        (cls.dir / "sugar.aura").write_text(PROG_SUGAR)
        (cls.dir / "lambda.aura").write_text(PROG_LAMBDA)
        (cls.dir / "recover.aura").write_text(PROG_RECOVER)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def _run(self, args: list[str], program: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(AURA_BIN), *args, program],
            capture_output=True,
            text=True,
            env=_env(),
            cwd=self.dir,
            timeout=60,
            stdin=subprocess.DEVNULL,
        )

    def _assert_clean(self, p: subprocess.CompletedProcess[str]) -> dict[str, str]:
        blob = p.stdout + p.stderr
        self.assertEqual(p.returncode, 0, blob)
        self.assertNotIn("densify-throttle", blob)
        self.assertNotIn("Moving densify window incomplete", blob)
        self.assertNotIn("<unknown>", blob)
        return _lines(p.stdout)

    def test_define_sugar_repro(self) -> None:
        for name, args in SESSIONS.items():
            with self.subTest(session=name):
                got = self._assert_clean(self._run(args, "sugar.aura"))
                self.assertNotEqual(got.get("snapshot"), "-1", got)
                self.assertTrue(got.get("snapshot", "-1").lstrip("-").isdigit(), got)
                self.assertGreaterEqual(int(got["snapshot"]), 0, got)
                self.assertEqual(got.get("rebind"), "#t", got)
                self.assertEqual(got.get("call"), "(1)", got)

    def test_lambda_workspace(self) -> None:
        for name, args in SESSIONS.items():
            with self.subTest(session=name):
                got = self._assert_clean(self._run(args, "lambda.aura"))
                self.assertGreaterEqual(int(got["s0"]), 0, got)
                self.assertEqual(got.get("rb"), "#t", got)
                self.assertEqual(got.get("call"), "#t", got)
                self.assertEqual(got.get("mapped"), "(1)", got)
                self.assertEqual(got.get("reason"), ":none", got)

    def test_refused_mutate_then_admits(self) -> None:
        for name, args in SESSIONS.items():
            with self.subTest(session=name):
                got = self._assert_clean(self._run(args, "recover.aura"))
                self.assertEqual(got.get("bad"), "-1", got)
                self.assertEqual(got.get("reason"), ":no-workspace", got)
                self.assertEqual(got.get("kw"), ":guard-reject", got)
                self.assertEqual(got.get("mapped"), "(1)", got)
                self.assertGreaterEqual(int(got["s0"]), 0, got)
                self.assertEqual(got.get("rb"), "#t", got)
                self.assertEqual(got.get("call"), "#t", got)
                self.assertGreaterEqual(int(got["s1"]), 0, got)
                self.assertEqual(got.get("rest"), "#t", got)
                self.assertEqual(got.get("after"), "#f", got)
                self.assertEqual(got.get("reason2"), ":none", got)


if __name__ == "__main__":
    unittest.main()
