#!/usr/bin/env python3
"""Regression for #5037: :doc, the error caret, and Ctrl-R history search.

Before: :help was the only command help, an eval error printed the whole
line without marking the column, and linenoise had no Ctrl-R.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import pexpect

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

C_R = "\x12"


def env(home: str | None = None) -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    if home:
        e["HOME"] = home
    return e


class Doc(unittest.TestCase):
    def run_repl(self, stdin: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(AURA_BIN), "repl", "--quiet"],
            input=stdin,
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )

    def test_doc_prints_the_primitive_description(self) -> None:
        r = self.run_repl(":doc car\n(quit)\n")
        self.assertIn("First element of a pair.", r.stdout, r.stdout + r.stderr)

    def test_doc_of_an_unknown_name_points_at_type(self) -> None:
        r = self.run_repl(":doc no-such-prim\n(quit)\n")
        self.assertIn("no such primitive", r.stdout)
        self.assertIn(":type", r.stdout)


class ErrorCaret(unittest.TestCase):
    def test_error_marks_the_column(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(r"> ")
            child.sendline("(no-such-fn 1)")
            child.expect(r"\^")
            out = child.before
        finally:
            child.sendline("(quit)")
            child.close(force=True)
        # The caret sits under the offending name, not at column 1.
        self.assertIn("   |", out, out)


class ReverseSearch(unittest.TestCase):
    def test_ctrl_r_finds_an_older_line(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            child = pexpect.spawn(
                str(AURA_BIN),
                ["repl"],
                timeout=30,
                env=env(d),
                encoding="utf-8",
                codec_errors="replace",
            )
            try:
                child.expect(r"> ")
                child.sendline("42")
                child.expect("= 42")
                child.send(C_R)
                child.send("42")
                child.send("\r")  # accept the match
                child.send("\r")  # evaluate it
                child.expect("= 42")
            finally:
                child.send("(quit)\r")
                child.close(force=True)


if __name__ == "__main__":
    unittest.main()
