#!/usr/bin/env python3
"""Regression for #5045: :context is one value for the next round."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN, repl_record  # noqa: E402


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    return e


def run_repl(stdin: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(AURA_BIN), "repl", "--quiet"],
        input=stdin,
        capture_output=True,
        text=True,
        env=env(),
        timeout=120,
    )


def contexts(result: subprocess.CompletedProcess[str]) -> list[dict]:
    return [repl_record(x) for x in result.stdout.splitlines() if x.startswith('(hash "name"')]


class Context(unittest.TestCase):
    def test_error_after_a_failed_round(self) -> None:
        r = run_repl('(set-code "(define (guess) 0)")\n:focus guess\n(no-such-fn)\n:context\n(quit)\n')
        ctx = contexts(r)[-1]
        self.assertEqual(ctx.get("name"), "guess", ctx)
        self.assertEqual(ctx.get("error"), "no-such-fn", ctx)

    def test_keep_updates_source_and_worldline(self) -> None:
        r = run_repl('(set-code "(define (guess) 0)")\n:propose guess "(define (guess) 7)"\n:context\n(quit)\n')
        ctx = contexts(r)[-1]
        self.assertIn("(lambda () 7)", ctx.get("source", ""), ctx)
        self.assertEqual(ctx.get("error"), "#f", ctx)
        self.assertIn("KEEP", ctx.get("worldline", ""), ctx)
        self.assertIn("(lambda () 7)", ctx.get("worldline", ""), ctx)

    def test_no_focus_reports_hash_false(self) -> None:
        r = run_repl(":context\n(quit)\n")
        ctx = contexts(r)[-1]
        self.assertEqual(ctx.get("name"), "#f", ctx)
        self.assertEqual(ctx.get("source"), "#f", ctx)

    def test_context_is_one_line(self) -> None:
        r = run_repl('(set-code "(define (guess) 0)")\n:focus guess\n:context\n(quit)\n')
        lines = [x for x in r.stdout.splitlines() if x.startswith('(hash "name"')]
        self.assertEqual(len(lines), 1, r.stdout)


if __name__ == "__main__":
    unittest.main()
