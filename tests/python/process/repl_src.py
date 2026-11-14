#!/usr/bin/env python3
"""Regression for #5042: :src reads the live source, :propose gates a rewrite.

Fuel/interrupt are tracked separately; this file locks the source page,
the KEEP / ROLLBACK decision, the record fields and the no-focus path.
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import pexpect

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN, repl_record  # noqa: E402


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    return e


def run_repl(stdin: str, cwd: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(AURA_BIN), "repl", "--quiet"],
        input=stdin,
        capture_output=True,
        text=True,
        env=env(),
        cwd=cwd,
        timeout=120,
    )


class SourcePage(unittest.TestCase):
    def test_src_reads_live_source_not_a_file(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            r = run_repl(
                '(set-code "(define (guess) 0)")\n(mutate :rebind "guess" "(lambda () 7)" "test")\n:src guess\n(quit)\n',
                cwd=d,
            )
        self.assertIn("(define guess (lambda () 7))", r.stdout, r.stdout + r.stderr)

    def test_src_without_a_focus_is_reported(self) -> None:
        r = run_repl(":src\n(quit)\n")
        self.assertIn("no-focus", r.stdout)


class Propose(unittest.TestCase):
    def test_keep_updates_the_source(self) -> None:
        r = run_repl('(set-code "(define (guess) 0)")\n:propose guess "(define (guess) 7)"\n:src guess\n(quit)\n')
        self.assertIn("KEEP", r.stdout)
        self.assertIn("(define guess (lambda () 7))", r.stdout)

    def test_failed_gate_rolls_back(self) -> None:
        r = run_repl(
            '(set-code "(define (guess) 0)")\n'
            ':gate (= (guess) 7)\n:propose guess "(define (guess) 10)"\n:src guess\n(quit)\n'
        )
        self.assertIn("ROLLBACK", r.stdout)
        defined = [line for line in r.stdout.splitlines() if line.startswith("(define guess")]
        self.assertEqual(defined[-1], "(define guess (lambda () 0))", r.stdout)

    def test_fuel_exhaustion_rolls_back(self) -> None:
        r = run_repl(
            '(set-code "(define guess 0)")\n'
            ":fuel 5000\n"
            ':propose guess "(define guess (let loop ((i 0)) (loop i)))"\n'
            ":src guess\n(quit)\n"
        )
        self.assertIn("ROLLBACK", r.stdout, r.stdout + r.stderr)
        defined = [line for line in r.stdout.splitlines() if line.startswith("(define guess")]
        self.assertEqual(defined[-1], "(define guess 0)", r.stdout)

    def test_ctrl_c_during_the_candidate_rolls_back(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(r"> ")
            child.sendline('(set-code "(define guess 0)")')
            child.expect(r"> ")
            child.sendline(":fuel 100000000")
            child.expect(r"> ")
            child.sendline(':propose guess "(define guess (let loop ((i 0)) (loop i)))"')
            time.sleep(1.0)
            child.kill(signal.SIGINT)
            child.expect("ROLLBACK")
            child.expect(r"> ")
            child.sendline(":src guess")
            child.expect(r"\(define guess 0\)")
        finally:
            child.sendline("(quit)")
            child.close(force=True)

    def test_pipe_record_carries_the_decision(self) -> None:
        r = run_repl('(set-code "(define (guess) 0)")\n:propose guess "(define (guess) 7)"\n(quit)\n')
        recs = [repl_record(x) for x in r.stdout.splitlines() if x.startswith("(hash")]
        last = recs[-1]
        self.assertEqual(last.get("decision"), "KEEP", last)
        self.assertIn("(lambda () 7)", last.get("source", ""), last)
        self.assertEqual(last.get("wrote"), '("guess")', last)

    def test_focus_prompt_counts_a_decision(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(r"> ")
            child.sendline(":focus guess")
            child.expect(r"guess 0> ")
            child.sendline('(set-code "(define (guess) 0)")')
            child.expect(r"guess 0> ")
            child.sendline(':propose guess "(define (guess) 7)"')
            child.expect(r"guess 1> ")
        finally:
            child.sendline("(quit)")
            child.close(force=True)


if __name__ == "__main__":
    unittest.main()
