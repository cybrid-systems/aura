#!/usr/bin/env python3
"""Focus and the prompt (#5048); the :src/:propose halves land with #5042."""

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


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    return e


class Prompt(unittest.TestCase):
    def test_focus_names_the_prompt(self) -> None:
        child = pexpect.spawn(str(AURA_BIN), ["repl"], timeout=30, env=env(), encoding="utf-8", codec_errors="replace")
        try:
            child.expect(r"> ")
            child.sendline(":focus guess")
            child.expect(r"guess 0> ")
            child.sendline(":focus")
            child.expect(r"guess 0")
            child.sendline("(current-source :workspace)")
            child.expect(r"guess 0> ")
        finally:
            child.sendline("(quit)")
            child.close(force=True)

    def test_src_without_a_name_uses_the_focus(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN), "repl", "--quiet"],
            input='(set-code "(define (guess) 0)")\n:focus guess\n:src\n(quit)\n',
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        self.assertIn("(define guess (lambda () 0))", r.stdout, r.stdout + r.stderr)

    def test_no_focus_is_reported(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN), "repl", "--quiet"],
            input=":focus\n(quit)\n",
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        self.assertIn("no-focus", r.stdout)

    def test_record_carries_the_focus(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN), "repl", "--quiet"],
            input="(+ 1 2)\n:focus guess\n(+ 1 2)\n(quit)\n",
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        recs = [repl_record(line) for line in r.stdout.splitlines() if line.startswith("(hash")]
        self.assertEqual(len(recs), 2, r.stdout)
        self.assertEqual(recs[0].get("focus"), "#f", recs[0])
        self.assertEqual(recs[1].get("focus"), "guess", recs[1])


if __name__ == "__main__":
    unittest.main()
