#!/usr/bin/env python3
"""Regression for #4869: force-soa --load top-level form splitting.

The splitter must follow the reader (character literals, strings, quoted
data, comments) and must never silently run a prefix of invalid source.
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


def _run(source: str, strict: str, timeout: float = 60.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = strict
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
        )
    finally:
        os.unlink(path)


class TestForceSoaSplit4869(unittest.TestCase):
    def test_character_literals_are_not_syntax(self) -> None:
        src = r"""
(define p #\()
(display (list 'char p)) (newline)
(display #\")
(newline)
(display #\;)
(newline)
(display "after")
(newline)
"""
        for strict in ("force-soa", "0"):
            p = _run(src, strict)
            self.assertEqual(p.returncode, 0, f"{strict}: {p.stderr}")
            self.assertIn("(char 40)", p.stdout)
            self.assertIn("34\n", p.stdout)
            self.assertIn("59\n", p.stdout)
            self.assertIn("after", p.stdout)

    def test_quoted_top_level_form_is_data(self) -> None:
        src = r"""
(define x 1)
'(display "SHOULD-NOT-PRINT")
(display (list 'x x)) (newline)
"""
        for strict in ("force-soa", "0"):
            p = _run(src, strict)
            self.assertEqual(p.returncode, 0, f"{strict}: {p.stderr}")
            self.assertEqual(p.stdout.strip(), "(x 1)")

    def test_unclosed_list_fails_loudly(self) -> None:
        src = r"""
(display "a")
(display (list 1 2)
(display "b")
"""
        p = _run(src, "force-soa")
        self.assertNotEqual(p.returncode, 0, p.stdout)
        self.assertIn("unclosed list", p.stderr)
        self.assertNotIn("a\n", p.stdout)

    def test_block_comment_is_skipped(self) -> None:
        # Issue #4875: a complete #| ... |# is a comment on every face;
        # the parens inside it no longer count as code.
        src = r"""
#| block comment with ( paren |#
(display "after-block") (newline)
"""
        for strict in ("force-soa", "0"):
            p = _run(src, strict)
            self.assertEqual(p.returncode, 0, f"{strict}: {p.stderr}")
            self.assertEqual(p.stdout.strip(), "after-block")
            self.assertNotIn("invalid character", p.stderr)

    def test_no_extra_last_value(self) -> None:
        src = r"""
(define x 1)
(+ x 1)
"""
        p = _run(src, "force-soa")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), "")


if __name__ == "__main__":
    unittest.main()
