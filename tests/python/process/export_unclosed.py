#!/usr/bin/env python3
"""Issue #5199: unclosed-list errors name the opening '(', not only EOF.

"(;" stays a real list (Scheme/Aura). The diagnostic points at that line and,
when the opener is immediately followed by ';', says the rest of the line was
commented out. A plain ';' header and a closed "(; comment\\n ...)" form still run.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

HINT = "'(' before ';' opened a real list and the rest of the line was commented out"
HEADER = "a file-header comment should be ';' without the leading '('"


def _write(source: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        return f.name


def _run(source: str, *, load: bool) -> subprocess.CompletedProcess[str]:
    path = _write(source)
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    env["AURA_PATH"] = str(ROOT / "lib")
    cmd = [str(AURA_BIN), "--load", path] if load else [str(AURA_BIN), path]
    try:
        return subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=60)
    finally:
        os.unlink(path)


PAREN_SEMI = """(; file header comment in Common Lisp style
(display "body ok")
(newline)
"""

MID = """(display "ok")
(newline)

(list 1
  2
"""

VALID = """(; comment about this form
  display "body ok")
"""

PLAIN = """; file header comment (plain semicolon)
(display "body ok")
(newline)
"""


class TestUnclosedList5199(unittest.TestCase):
    def test_paren_semi_file_names_line_1(self) -> None:
        # Direct file face (split_oneshot_forms). Before this change:
        # error: 4:1: parse error: unclosed list: missing ')' before end of file
        p = _run(PAREN_SEMI, load=False)
        self.assertNotEqual(p.returncode, 0, p.stdout)
        self.assertIn("error: 1:1:", p.stderr)
        self.assertIn("unclosed list opened at line 1", p.stderr)
        self.assertIn("column 1", p.stderr)
        self.assertIn(HINT, p.stderr)
        self.assertIn(HEADER, p.stderr)
        self.assertNotIn("error: 4:1:", p.stderr)
        self.assertNotIn("body ok", p.stdout)

    def test_paren_semi_load_names_line_1(self) -> None:
        p = _run(PAREN_SEMI, load=True)
        self.assertNotEqual(p.returncode, 0, p.stdout)
        self.assertIn(":1:1:", p.stderr)
        self.assertIn("unclosed list opened at line 1", p.stderr)
        self.assertIn(HINT, p.stderr)
        self.assertIn(HEADER, p.stderr)
        self.assertIn("(; file header comment", p.stderr)
        self.assertNotIn("error: 4:1:", p.stderr)
        self.assertNotIn(":4:1:", p.stderr)
        self.assertNotIn("body ok", p.stdout)

    def test_closed_paren_semi_runs(self) -> None:
        for load in (False, True):
            p = _run(VALID, load=load)
            self.assertEqual(p.returncode, 0, f"load={load}: {p.stderr}")
            self.assertIn("body ok", p.stdout)
            self.assertNotIn("unclosed list", p.stderr)

    def test_plain_semicolon_header_runs(self) -> None:
        for load in (False, True):
            p = _run(PLAIN, load=load)
            self.assertEqual(p.returncode, 0, f"load={load}: {p.stderr}")
            self.assertIn("body ok", p.stdout)

    def test_mid_file_opener_is_not_eof(self) -> None:
        # Before: error: 6:1: ... (EOF). The unmatched '(' is line 4.
        for load in (False, True):
            p = _run(MID, load=load)
            self.assertNotEqual(p.returncode, 0, f"load={load}: {p.stdout}")
            self.assertIn("unclosed list opened at line 4", p.stderr)
            self.assertIn("4:1:", p.stderr)
            self.assertNotIn("6:1:", p.stderr)
            self.assertNotIn(HINT, p.stderr)
            self.assertNotIn("ok", p.stdout)


if __name__ == "__main__":
    unittest.main()
