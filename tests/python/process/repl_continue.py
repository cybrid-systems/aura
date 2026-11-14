#!/usr/bin/env python3
"""Regression for #5039: an open phrase continues on a .. prompt.

Before: every Enter evaluated the line, so (define (f x) failed and the
next line was evaluated as a fragment. Pipe mode keeps the #2292
one-line-in/one-line-out contract.
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

from _aura_harness import AURA_BIN, repl_value  # noqa: E402


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    return e


class Continuation(unittest.TestCase):
    def setUp(self) -> None:
        self.child = pexpect.spawn(
            str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace"
        )
        self.child.expect(r"> ")

    def tearDown(self) -> None:
        self.child.sendline("(quit)")
        self.child.close(force=True)

    def test_two_line_define_evaluates_once(self) -> None:
        self.child.sendline("(define (f x)")
        self.child.expect(r"\.\. ")
        self.child.sendline("  (+ x 1))")
        self.child.expect(r"> ")
        self.child.sendline("(f 41)")
        self.child.expect("42")

    def test_paren_inside_string_is_complete(self) -> None:
        self.child.sendline('(display "(")')
        self.child.expect(r"> ")

    def test_paren_inside_comment_is_complete(self) -> None:
        self.child.sendline("; (")
        self.child.expect(r"> ")

    def test_open_string_continues(self) -> None:
        self.child.sendline('(display "abc')
        self.child.expect(r"\.\. ")
        self.child.sendline('def")')
        self.child.expect(r"> ")


class PipeUnchanged(unittest.TestCase):
    def test_pipe_is_still_one_line_in_one_line_out(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN), "repl"],
            input="(+ 1 2)\n(+ 3 4)\n(quit)\n",
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        self.assertEqual([repl_value(line) for line in r.stdout.splitlines()], ["3", "7"])


if __name__ == "__main__":
    unittest.main()
