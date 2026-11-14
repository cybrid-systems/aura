#!/usr/bin/env python3
"""Regression for #5036: the aura repl banner drifted from the CLI.

Before: the TTY banner was a hand-written "Aura v0.2 - LLVM JIT / Sound
Gradual Typing / C FFI". aura --version prints "aura dev (<sha>)", and
aura --help calls the product "an AI-native Lisp", so the banner carried
a fake version and a tagline the CLI never used.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

import pexpect  # noqa: E402
from _aura_harness import AURA_BIN  # noqa: E402

TAGLINE = "an AI-native Lisp"


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_VERSION"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    return e


def version_token() -> str:
    r = subprocess.run([str(AURA_BIN), "--version"], capture_output=True, text=True, env=env(), timeout=120)
    m = re.match(r"^aura (\S+)", r.stdout)
    assert m, r.stdout + r.stderr
    return m.group(1)


def help_first_line() -> str:
    r = subprocess.run([str(AURA_BIN), "--help"], capture_output=True, text=True, env=env(), timeout=120)
    return (r.stdout or r.stderr).splitlines()[0]


class Banner(unittest.TestCase):
    def test_tty_banner_matches_version_and_help(self) -> None:
        expected = f"aura {version_token()} \u2014 {TAGLINE}"
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=60, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(re.escape(expected))
        finally:
            child.sendline("(quit)")
            child.close(force=True)
        self.assertEqual(expected, help_first_line())
        self.assertNotIn("Aura v0.2", expected)
        self.assertNotIn("LLVM JIT", expected)

    def test_pipe_repl_prints_no_banner(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN), "repl"],
            input="(quit)\n",
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        combined = r.stdout + r.stderr
        self.assertNotIn(TAGLINE, combined)
        self.assertNotIn("Aura v0.2", combined)


if __name__ == "__main__":
    unittest.main()
