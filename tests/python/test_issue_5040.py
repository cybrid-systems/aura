#!/usr/bin/env python3
"""Regression for #5040: Tab completes the word at the cursor.

Before: the completion callback had no caller, so Tab offered nothing.
Candidates are session bindings + prelude/required exports + primitives +
`:commands`, and std/ module names inside (require "...").
"""

from __future__ import annotations

import os
import sys
import time
import unittest
from pathlib import Path

import pexpect

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

TAB = "\t"
C_U = "\x15"


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "xterm-256color"
    return e


class Completion(unittest.TestCase):
    def setUp(self) -> None:
        # cwd is the repo root so lib/std is discoverable.
        self.child = pexpect.spawn(
            str(AURA_BIN),
            ["repl"],
            timeout=30,
            env=env(),
            cwd=str(ROOT),
            encoding="utf-8",
            codec_errors="replace",
        )
        self.child.expect(r"> ")

    def tearDown(self) -> None:
        self.child.send("(quit)\r")
        self.child.close(force=True)

    def tab(self, text: str) -> str:
        for ch in text:
            self.child.send(ch)
        self.child.send(TAB)
        time.sleep(0.3)
        out = self.child.read_nonblocking(size=2000, timeout=2)
        self.child.send(C_U)
        return out

    def test_word_at_cursor_keeps_the_head(self) -> None:
        out = self.tab("(map ca")
        self.assertIn("(map caa", out)

    def test_command_completion(self) -> None:
        out = self.tab(":lo")
        self.assertIn(":load", out)

    def test_prelude_export_completion(self) -> None:
        out = self.tab("inter")
        self.assertIn("intersperse", out)

    def test_module_name_inside_require(self) -> None:
        out = self.tab('(require "std/j')
        self.assertIn("std/json", out)

    def test_exports_follow_require(self) -> None:
        before = self.tab("json-arr")
        self.assertNotIn("json-arr-items", before)
        self.child.send('(require "std/json")\r')
        self.child.expect(r"> ")
        after = self.tab("json-arr")
        self.assertIn("json-arr-items", after)


if __name__ == "__main__":
    unittest.main()
