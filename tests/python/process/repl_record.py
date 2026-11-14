#!/usr/bin/env python3
"""Regression for #5043: one evaluation answers one record on a pipe.

Before: a value, a define and a failure were three different stdout
shapes (a line, an empty line, stderr plus an empty line). The pipe now
prints exactly one (hash "status" ...) line so a driver can tell them apart.
"""

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


def env(extra: dict[str, str] | None = None) -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    if extra:
        e.update(extra)
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


class PipeRecords(unittest.TestCase):
    def test_value_record(self) -> None:
        r = run_repl("(+ 1 2)\n(quit)\n")
        lines = r.stdout.splitlines()
        self.assertEqual(len(lines), 1, r.stdout + r.stderr)
        rec = repl_record(lines[0])
        self.assertEqual(rec.get("status"), "ok", rec)
        self.assertEqual(rec.get("value"), "3", rec)
        self.assertEqual(rec.get("wrote"), "()", rec)
        self.assertTrue(rec.get("type"), rec)

    def test_define_record_is_void(self) -> None:
        r = run_repl("(define (guess) 42)\n(quit)\n")
        rec = repl_record(r.stdout.splitlines()[0])
        self.assertEqual(rec.get("status"), "ok", rec)
        self.assertEqual(rec.get("value"), "", rec)
        self.assertEqual(rec.get("type"), "void", rec)
        self.assertEqual(rec.get("wrote"), '("guess")', rec)

    def test_error_record_without_stderr(self) -> None:
        r = run_repl("(no-such-fn 1)\n(quit)\n")
        rec = repl_record(r.stdout.splitlines()[0])
        self.assertEqual(rec.get("status"), "error", rec)
        self.assertTrue(rec.get("message"), rec)
        self.assertEqual(rec.get("line"), "1", rec)
        self.assertIn("column", rec)
        self.assertEqual(r.stderr.strip(), "", r.stderr)

    def test_ok_and_error_are_one_line_each(self) -> None:
        r = run_repl("(+ 1 2)\n(no-such-fn)\n(define x 1)\n(quit)\n")
        recs = [repl_record(line) for line in r.stdout.splitlines()]
        self.assertEqual([x.get("status") for x in recs], ["ok", "error", "ok"], r.stdout)


class TtyUnchanged(unittest.TestCase):
    def test_tty_prints_the_human_line(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=60, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(r"> ")
            child.sendline("(+ 1 2)")
            child.expect("3")
            child.sendline("(quit)")
            child.expect(pexpect.EOF)
        finally:
            child.close(force=True)
        self.assertNotIn("(hash", child.before or "")

    def test_tty_record_env(self) -> None:
        child = pexpect.spawn(
            str(AURA_BIN),
            ["repl"],
            timeout=60,
            env=env({"AURA_REPL_RECORD": "1"}),
            encoding="utf-8",
            codec_errors="replace",
        )
        try:
            child.expect(r"> ")
            child.sendline("(+ 1 2)")
            child.expect(r"\"value\" \"3\"")
        finally:
            child.sendline("(quit)")
            child.close(force=True)


if __name__ == "__main__":
    unittest.main()
