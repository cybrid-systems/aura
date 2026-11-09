#!/usr/bin/env python3
"""Regression for #4909: a program file stops at its first top-level error.

Before: after `(error "x must be >= 0")` the file kept running ("still
running" printed, later forms ran), so one mistake produced a cascade of
follow-on errors and the first one was buried. Now a program file prints the
first error once, runs nothing after it, and exits 1. `--keep-going` (or
AURA_KEEP_GOING=1) keeps the run-every-form contract, and -e / stdin keep it
as well. Also: `(query :code)` names the `query:code` primitive.
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

PROG = (
    '(define (f x) (if (< x 0) (error "x must be >= 0") x))\n'
    '(display "a") (newline)\n'
    "(f -1)\n"
    '(display "still running") (newline)\n'
    "(define y 3)\n"
    "(display y) (newline)\n"
)


def _run(args, src, strict=None, env_extra=None, stdin=False):
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_KEEP_GOING", None)
    env.pop("AURA_PIPELINE_STRICT", None)
    if strict is not None:
        env["AURA_PIPELINE_STRICT"] = strict
    env.update(env_extra or {})
    if stdin:
        return subprocess.run([str(AURA_BIN), *args], input=src, capture_output=True, text=True, env=env, timeout=60)
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        return subprocess.run([str(AURA_BIN), *args, path], capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


class FirstErrorStops(unittest.TestCase):
    def test_file_stops_at_first_error(self) -> None:
        for strict in (None, "0", "force-soa"):
            with self.subTest(strict=strict):
                p = _run([], PROG, strict=strict)
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertEqual(p.stdout, "a\n", p.stderr)
                self.assertEqual(p.stderr.count(": error: x must be >= 0"), 1, p.stderr)
                self.assertNotIn("unbound variable", p.stderr)

    def test_keep_going_flag(self) -> None:
        for strict in (None, "force-soa"):
            with self.subTest(strict=strict):
                p = _run(["--keep-going"], PROG, strict=strict)
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertEqual(p.stdout, "a\nstill running\n3\n", p.stderr)
                self.assertEqual(p.stderr.count(": error: x must be >= 0"), 1, p.stderr)

    def test_keep_going_env(self) -> None:
        p = _run([], PROG, env_extra={"AURA_KEEP_GOING": "1"})
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(p.stdout, "a\nstill running\n3\n", p.stderr)

    def test_stdin_keeps_running(self) -> None:
        p = _run([], PROG, stdin=True)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(p.stdout, "a\nstill running\n3\n", p.stderr)

    def test_query_ops_dispatch_and_reject_unknown(self) -> None:
        # Issue #5009 replaced the query:<name> surface with (query :op):
        # :code is now a valid op that dispatches to the query:code primitive.
        p = _run([], "(display (query :code))\n")
        out = p.stdout + p.stderr
        self.assertIn("no-workspace", out)
        self.assertNotIn("unknown query op", out)

        p = _run([], "(display (query :zzz))\n")
        out = p.stdout + p.stderr
        self.assertIn("unknown query op", out)


if __name__ == "__main__":
    unittest.main()
