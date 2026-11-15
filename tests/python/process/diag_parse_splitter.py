#!/usr/bin/env python3
"""Parser-owned parse diagnostics: unterminated strings, hints and REPL wrote."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def run(args: list[str], cwd: str | None = None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    return subprocess.run([str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120)


class ParseSplitter(unittest.TestCase):
    def test_unterminated_string_every_file_mode(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            p.write_text('(display "abc)\n')
            for args in ([str(p)], ["--load", str(p)], ["--preset", "dev", "--load", str(p)]):
                r = run(args, cwd=d)
                self.assertNotEqual(r.returncode, 0, args)
                self.assertIn("unterminated string literal", r.stderr, args)
                self.assertNotIn("unclosed list", r.stderr, args)
                self.assertIn("u.aura:1:10", r.stderr, args)
                self.assertIn("abc", r.stderr, args)  # snippet

    def test_unterminated_string_e_and_typecheck(self) -> None:
        r = run(["-e", '(display "abc)'])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unterminated string literal", r.stdout + r.stderr)
        self.assertNotIn("unclosed list", r.stdout + r.stderr)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            p.write_text('(display "abc)\n')
            r = run(["--typecheck", str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("unterminated string literal", r.stdout + r.stderr)
            self.assertNotIn("unclosed list", r.stdout + r.stderr)

    def test_unterminated_string_serve(self) -> None:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        p = subprocess.run(
            [str(AURA_BIN), "--serve"], input='(display "abc)\n', capture_output=True, text=True, env=env, timeout=120
        )
        replies = [json.loads(ln) for ln in p.stdout.splitlines() if ln.strip()]
        self.assertTrue(replies, p.stderr)
        self.assertEqual(replies[-1]["status"], "error")
        self.assertIn("unterminated string literal", replies[-1]["msg"])
        self.assertNotIn("unclosed list", replies[-1]["msg"])

    def test_header_parenthesis_hint_survives(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "s.aura"
            p.write_text("(; header\n(display 1)\n")
            r = run([str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("'(' before ';' opened a real list", r.stderr)

    def test_repl_unclosed_define_wrote_nothing(self) -> None:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        p = subprocess.run(
            [str(AURA_BIN), "repl"], input="(define (k x)\n", capture_output=True, text=True, env=env, timeout=120
        )
        rec = [ln for ln in p.stdout.splitlines() if ln.startswith("(hash")]
        self.assertTrue(rec, p.stdout + p.stderr)
        self.assertIn('"status" "error"', rec[-1])
        self.assertIn('"wrote" ()', rec[-1], rec[-1])


if __name__ == "__main__":
    unittest.main()
