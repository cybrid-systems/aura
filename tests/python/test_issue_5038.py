#!/usr/bin/env python3
"""Regression for #5038: the REPL shows types, :type, and a grey hint.

Before: a value printed without its type, there was no :type command, and
linenoise's hints callback had no caller.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

import pexpect

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN, repl_record  # noqa: E402


def env(term: str = "dumb") -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = term
    return e


class TypedResults(unittest.TestCase):
    def setUp(self) -> None:
        self.child = pexpect.spawn(
            str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace"
        )
        self.child.expect(r"> ")

    def tearDown(self) -> None:
        self.child.sendline("(quit)")
        self.child.close(force=True)

    def round(self, line: str) -> str:
        self.child.sendline(line)
        self.child.expect(r"> ")
        return self.child.before

    def test_value_carries_its_type(self) -> None:
        out = self.round("(+ 1 2)")
        self.assertIn("- : Int = 3", out)

    def test_type_command_does_not_evaluate(self) -> None:
        out = self.round(':type (display "SIDE")')
        self.assertIn("- : Void", out)
        self.assertEqual(out.count("SIDE"), 1, out)  # the echo only

    def test_type_of_a_primitive(self) -> None:
        out = self.round(":type car")
        self.assertIn("- : car/1", out)

    def test_pipe_record_keeps_the_same_type(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN), "repl", "--quiet"],
            input="(+ 1 2)\n(quit)\n",
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        rec = repl_record(r.stdout.splitlines()[0])
        self.assertEqual(rec.get("type"), "Int", rec)
        self.assertEqual(rec.get("value"), "3", rec)


class Hints(unittest.TestCase):
    # Hints are drawn through the terminfo path, so a real TERM is needed.
    def spawn(self):
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env("xterm-256color"), encoding="utf-8")
        child.expect(r"> ")
        return child

    def test_primitive_hint_shows_arity(self) -> None:
        child = self.spawn()
        try:
            child.send("(car")
            child.expect("car/1")
        finally:
            child.sendline("")
            child.sendline("(quit)")
            child.close(force=True)

    def test_unknown_word_has_no_hint(self) -> None:
        child = self.spawn()
        try:
            child.send("(zzzq")
            with self.assertRaises(pexpect.TIMEOUT):
                child.expect("zzzq/", timeout=1.0)
        finally:
            child.sendline("")
            child.sendline("(quit)")
            child.close(force=True)


if __name__ == "__main__":
    unittest.main()
