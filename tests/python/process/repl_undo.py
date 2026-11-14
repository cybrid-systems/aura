#!/usr/bin/env python3
"""Regression for #5047: :undo / :redo / :at walk one name's worldline."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

import pexpect

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN, repl_record  # noqa: E402

BASE = '(set-code "(define (guess) 0)")\n:focus guess\n'


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


def records(result: subprocess.CompletedProcess[str]) -> list[dict]:
    return [repl_record(x) for x in result.stdout.splitlines() if x.startswith("(hash")]


class Worldline(unittest.TestCase):
    def test_undo_returns_to_the_first_decision(self) -> None:
        r = run_repl(
            BASE
            + ':propose guess "(define (guess) 7)"\n'
            + ':propose guess "(define (guess) 9)"\n:undo\n:src guess\n(quit)\n'
        )
        last = records(r)[-1]
        self.assertEqual(last.get("decision"), "undo", last)
        self.assertIn("(lambda () 7)", last.get("source", ""), last)
        self.assertIn("(define guess (lambda () 7))", r.stdout)

    def test_at_does_not_change_the_source(self) -> None:
        r = run_repl(
            BASE
            + ':propose guess "(define (guess) 7)"\n'
            + ':propose guess "(define (guess) 9)"\n:at 1\n:src guess\n(quit)\n'
        )
        recs = records(r)
        at = [x for x in recs if "decision" not in x][-1]
        self.assertIn("(lambda () 7)", at.get("source", ""), at)
        self.assertEqual(at.get("wrote"), "()", at)
        self.assertIn("(define guess (lambda () 9))", r.stdout)  # still the live source

    def test_undo_at_step_zero_is_a_no_op(self) -> None:
        r = run_repl(BASE + ':propose guess "(define (guess) 7)"\n:undo\n:undo\n:src guess\n(quit)\n')
        self.assertEqual(r.stdout.count("no-undo"), 1, r.stdout)
        self.assertIn("(define guess (lambda () 0))", r.stdout)

    def test_new_propose_drops_the_redo_tail(self) -> None:
        r = run_repl(
            BASE
            + ':propose guess "(define (guess) 7)"\n:undo\n'
            + ':propose guess "(define (guess) 5)"\n:redo\n(quit)\n'
        )
        self.assertIn("no-redo", r.stdout)

    def test_prompt_counts_undo_as_a_step(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(r"> ")
            child.sendline('(set-code "(define (guess) 0)")')
            child.expect(r"> ")
            child.sendline(":focus guess")
            child.expect(r"guess 0> ")
            child.sendline(':propose guess "(define (guess) 7)"')
            child.expect(r"guess 1> ")
            child.sendline(":undo")
            child.expect(r"guess 2> ")
        finally:
            child.sendline("(quit)")
            child.close(force=True)


if __name__ == "__main__":
    unittest.main()
