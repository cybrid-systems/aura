#!/usr/bin/env python3
"""Regression for #4875 (rest): block comment / stray |# reader gaps.

On top of the #| / #; / bare # consistency already pinned by
test_issue_4875_reader_hash.py, every face (default and force-soa, --load
and direct) must agree on:

  G  a ( or " inside a #| ... |# must not move the direct-run paren split
  H  block comments nest (R7RS 2.2): #| x #| y |# z |# is one comment
  S  a stray |# (no open #|) is a located parse error; it used to read as
     an unterminated |symbol| and drop the rest of the file with rc=0
  U  an unterminated |symbol| is a parse error, not silent
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

FACES = (("load", "0"), ("load", "force-soa"), ("direct", "0"), ("direct", "force-soa"))

SOURCES = {
    "G": '(display "a") (newline)\n#| a ( b " |#\n(display "b") (newline)\n',
    "G2": '(display (list 1 #| c ( |# 2)) (newline)\n(display "b") (newline)\n',
    "H": '(display "a") (newline)\n#| x #| nested ( |# y |#\n(display "b") (newline)\n',
    "S": '(display "a") (newline)\n|#\n(display "b") (newline)\n',
    "U": '(display "a") (newline)\n(display (quote |abc))\n(display "b") (newline)\n',
}


def _run(source: str, entry: str, strict: str):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = strict
    argv = [str(AURA_BIN)] + (["--load", path] if entry == "load" else [path])
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


class TestReaderNested4875(unittest.TestCase):
    def test_comment_contents_are_not_code(self) -> None:
        expected = {"G": "a\nb", "G2": "(1 2)\nb", "H": "a\nb"}
        for key, out in expected.items():
            for entry, strict in FACES:
                with self.subTest(case=key, entry=entry, strict=strict):
                    p = _run(SOURCES[key], entry, strict)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    self.assertEqual(p.stdout.strip(), out, p.stdout)
                    self.assertEqual(p.stderr.strip(), "")

    def test_stray_close_is_a_located_error(self) -> None:
        for entry, strict in FACES:
            with self.subTest(entry=entry, strict=strict):
                p = _run(SOURCES["S"], entry, strict)
                self.assertNotEqual(p.returncode, 0, p.stdout)
                self.assertIn("unexpected '|#' outside a block comment", p.stderr)
                self.assertIn("2:1", p.stderr)
                self.assertNotIn("b", p.stdout)

    def test_unterminated_pipe_symbol_is_an_error(self) -> None:
        for entry, strict in FACES:
            with self.subTest(entry=entry, strict=strict):
                p = _run(SOURCES["U"], entry, strict)
                self.assertNotEqual(p.returncode, 0, p.stdout)
                self.assertIn("unterminated |symbol|", p.stderr)


if __name__ == "__main__":
    unittest.main()
