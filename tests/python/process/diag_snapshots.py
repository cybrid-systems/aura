#!/usr/bin/env python3
"""Diagnostic snapshots for the five #4909 repros (rc, first error line, hint).

Each repro from the issue runs as a program file in the default sandbox. The
snapshot pins the exit code, the first `error:` line and the `hint:` / did-you-mean
line, and checks that nothing after the first error ran.
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

HINT = (
    "hint: run 'aura --preset dev p.aura' (or 'aura soft p.aura'; grants mutate on this workspace); "
    "for local development AURA_SANDBOX=off; in production grant the mutate effect"
)
MEMQ = "did you mean 'member'? (Aura's member compares with equal?; memq/memv are not provided)"

# name -> (source, rc, first error line, hint line or None, text that must not be printed)
SNAPSHOTS: dict[str, tuple[str, int, str | None, str | None, str | None]] = {
    "1-set-code-denied": (
        "(define (g) (+ 1 2))\n(set-code 'g '(lambda () 5))\n(display \"after\")\n",
        1,
        "error: p.aura:2:2: effect-denied: mutate not granted tenant=0 op=set-code",
        HINT,
        "after",
    ),
    "1-set-car-denied": (
        '(define p (cons 1 2))\n(set-car! p 9)\n(display "after")\n',
        1,
        "error: p.aura:2:2: effect-denied: mutate not granted tenant=0 op=set-car!",
        HINT,
        "after",
    ),
    "2-user-error": (
        '(define (f x) (if (< x 0) (error "x must be >= 0") x))\n(f -1)\n(display "still running")\n',
        1,
        "error: p.aura:1:28: error: x must be >= 0",
        None,
        "still running",
    ),
    "3-no-cascade": (
        '(define (bad) (error "boom"))\n(bad)\n(define a 1)\n(define b (+ a 1))\n(display b)\n',
        1,
        "error: p.aura:1:16: error: boom",
        None,
        "unbound",
    ),
    "3-head-define-error-value": (
        '(define (f z)\n  (car z))\n(define r (f 5))\n(display "after")\n',
        1,
        "error: p.aura:2:4: type error: car: not a pair",
        None,
        "after",
    ),
    "4-require-std": ('(require "std/process")\n(require "std/fs")\n(display "loaded")\n', 0, None, None, "error"),
    "5-memq": (
        "(display (memq 1 (list 1 2)))\n",
        1,
        "error: p.aura:1:11: unbound variable: memq",
        MEMQ,
        None,
    ),
}


def run_prog(src: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "p.aura").write_text(src)
        env = dict(os.environ)
        for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING", "AURA_GRANT"):
            env.pop(k, None)
        return subprocess.run([str(AURA_BIN), "p.aura"], cwd=d, capture_output=True, text=True, env=env, timeout=120)


class Snapshots(unittest.TestCase):
    def test_snapshots(self) -> None:
        for name, (src, rc, first, hint, absent) in SNAPSHOTS.items():
            with self.subTest(name):
                r = run_prog(src)
                out = r.stdout + r.stderr
                self.assertEqual(r.returncode, rc, out)
                errs = [ln for ln in r.stderr.splitlines() if ln.startswith("error:")]
                if first is None:
                    self.assertEqual(errs, [], out)
                else:
                    self.assertEqual(errs, [first], out)
                    lines = r.stderr.splitlines()
                    self.assertEqual(lines[1].strip() if hint else None, hint, out)
                if absent:
                    self.assertNotIn(absent, r.stdout, out)


if __name__ == "__main__":
    unittest.main()
