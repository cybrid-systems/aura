#!/usr/bin/env python3
"""Regression for #5046: rewrites print a four-line source diff.

name / `-` old live source / `+` new source / decision, on a TTY only;
the pipe keeps one record per round and adds `previous`.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import pexpect

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN, repl_record  # noqa: E402


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    return e


class ProposeDiff(unittest.TestCase):
    def test_keep_prints_the_block(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(r"> ")
            child.sendline('(set-code "(define guess 0)")')
            child.expect(r"> ")
            child.sendline(':propose guess "(define (guess) 7)"')
            child.expect(r"KEEP")
            out = child.before
        finally:
            child.sendline("(quit)")
            child.close(force=True)
        self.assertIn("guess", out)
        self.assertIn("- (define guess 0)", out)
        self.assertIn("+ (define (guess) 7)", out)

    def test_rollback_prints_the_block(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(r"> ")
            child.sendline('(set-code "(define guess 0)")')
            child.expect(r"> ")
            child.sendline(":gate (= (guess) 7)")
            child.expect(r"> ")
            child.sendline(':propose guess "(define (guess) 10)"')
            child.expect(r"ROLLBACK")
            out = child.before
        finally:
            child.sendline("(quit)")
            child.close(force=True)
        self.assertIn("- (define guess 0)", out)
        self.assertIn("+ (define (guess) 10)", out)

    def test_pipe_record_has_previous_and_no_block(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN), "repl", "--quiet"],
            input='(set-code "(define guess 0)")\n:propose guess "(define (guess) 7)"\n(quit)\n',
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        self.assertNotIn("- (define", r.stdout)
        recs = [repl_record(x) for x in r.stdout.splitlines() if x.startswith("(hash")]
        last = recs[-1]
        self.assertEqual(last.get("decision"), "KEEP", last)
        self.assertIn("(lambda () 7)", last.get("source", ""), last)
        self.assertIn("(define guess 0)", last.get("previous", ""), last)


class ReloadDiff(unittest.TestCase):
    def test_file_only_define_marks_the_block(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "lib.aura"
            f.write_text("(define other 1)\n")
            child = pexpect.spawn(
                str(AURA_BIN),
                ["repl"],
                timeout=30,
                env=env(),
                cwd=d,
                encoding="utf-8",
                codec_errors="replace",
            )
            try:
                child.expect(r"> ")
                child.sendline('(set-code "(define (guess) 0)")')
                child.expect(r"> ")
                child.sendline(":load lib.aura")
                child.expect(r"> ")
                f.write_text("(define other 2)\n")
                child.sendline(":reload")
                child.expect(r"unchanged 0")
                out = child.before
            finally:
                child.sendline("(quit)")
                child.close(force=True)
            self.assertIn("- (define other 1)", out)
            self.assertIn("+ (define other 2)", out)
            self.assertIn("KEEP file", out)

    def test_one_block_per_changed_define(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "lib.aura"
            f.write_text("(define (score) 1)\n(define other 2)\n")
            child = pexpect.spawn(
                str(AURA_BIN),
                ["repl"],
                timeout=30,
                env=env(),
                cwd=d,
                encoding="utf-8",
                codec_errors="replace",
            )
            try:
                child.expect(r"> ")
                child.sendline(":load lib.aura")
                child.expect(r"> ")
                f.write_text("(define (score) 9)\n(define other 2)\n")
                child.sendline(":reload")
                child.expect(r"unchanged 1")
                out = child.before
            finally:
                child.sendline("(quit)")
                child.close(force=True)
            self.assertIn("- (define", out)
            self.assertIn("+ (define", out)
            self.assertIn("KEEP", out)
            self.assertNotIn("- (define other", out)  # unchanged names stay out


if __name__ == "__main__":
    unittest.main()
