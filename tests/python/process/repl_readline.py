#!/usr/bin/env python3
"""Regression for #5041: readline emacs-mode keys in the REPL.

The buffer/cursor/kill-ring arithmetic is tested key by key in
tests/core/test_linenoise_core.cpp; this file proves the keys are wired
through linenoise on a pty: C-u, C-w, C-k, C-y and C-x C-u.
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

import pexpect

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

C_U = "\x15"
C_W = "\x17"
C_Y = "\x19"
C_K = "\x0b"
C_X = "\x18"
ESC = "\x1b"


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "xterm-256color"
    return e


class Keys(unittest.TestCase):
    def setUp(self) -> None:
        self.child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8")
        self.child.expect(r"> ")

    def tearDown(self) -> None:
        self.child.send("(quit)\r")
        self.child.close(force=True)

    def test_cu_kills_to_start_and_cy_pastes(self) -> None:
        self.child.send("42")
        self.child.send(C_U)
        self.child.send(C_Y)
        self.child.send("\r")
        self.child.expect("= 42")

    def test_cw_kills_the_word_into_the_ring(self) -> None:
        self.child.send("42")
        self.child.send(C_W)
        self.child.send(C_Y)
        self.child.send("\r")
        self.child.expect("= 42")

    def test_meta_b_then_ck_kills_only_the_tail(self) -> None:
        self.child.send("(car (list 1 2))")
        self.child.send(ESC + "b")
        self.child.send(C_K)
        self.child.send("\r")
        self.child.expect(r"\.\. ")  # the tail is gone: open phrase
        self.child.send(C_Y)
        self.child.send("\r")
        self.child.expect("= 1")

    def test_cx_cu_undoes_the_kill(self) -> None:
        self.child.send("42")
        self.child.send(C_U)
        self.child.send(C_X + C_U)
        self.child.send("\r")
        self.child.expect("= 42")


if __name__ == "__main__":
    unittest.main()
