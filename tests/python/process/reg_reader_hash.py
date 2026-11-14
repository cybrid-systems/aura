#!/usr/bin/env python3
"""Regression for #4875: reader consistency for #|, #; and a bare #.

Both faces (default and AURA_PIPELINE_STRICT=force-soa, via --load and the
direct file entry) must agree on the same reader:

  A  unterminated #| ...      -> located parse error, non-zero, and the code
                                 it swallows ((display "b")) never runs
  B  #| comment |# mid-file   -> whitespace, not a symbol
  C  #| c |# at file start    -> whitespace
  D  #; mid-file              -> explicit invalid-character error
  E  #; at file start         -> explicit invalid-character error
  F  bare #                   -> explicit invalid-character error

Errors must point at the real line/column (A is on line 2).
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

SOURCES = {
    "A": '(display "a") (newline)\n#| never closed\n(display "b")\n',
    "B": '(display "a") (newline)\n#| comment |#\n(display "b") (newline)\n',
    "C": '#| c |#\n(display "a") (newline)\n',
    "D": '(display "a") (newline)\n#;(display "x")\n(display "b") (newline)\n',
    "E": '#;(display "x")\n(display "a") (newline)\n',
    "F": '(display "a") (newline)\n#\n(display "b") (newline)\n',
}

# (entry, AURA_PIPELINE_STRICT) pairs: the issue's two modes x the two
# file entry points.
FACES = (
    ("load", "0"),
    ("load", "force-soa"),
    ("direct", "0"),
    ("direct", "force-soa"),
)


def _run(source: str, entry: str, strict: str, timeout: float = 60.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = strict
    argv = [str(AURA_BIN)]
    argv += ["--load", path] if entry == "load" else [path]
    try:
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=timeout)
    finally:
        os.unlink(path)


class TestReaderHash4875(unittest.TestCase):
    def test_unterminated_block_comment_is_a_located_error(self) -> None:
        for entry, strict in FACES:
            with self.subTest(entry=entry, strict=strict):
                p = _run(SOURCES["A"], entry, strict)
                self.assertNotEqual(p.returncode, 0, p.stdout)
                self.assertIn("unterminated block comment", p.stderr)
                self.assertIn("2:1", p.stderr, p.stderr)
                # The comment swallows (display "b"); it must not run.
                self.assertNotIn("b", p.stdout)

    def test_direct_face_keeps_the_valid_prefix(self) -> None:
        p = _run(SOURCES["A"], "direct", "0")
        self.assertNotEqual(p.returncode, 0, p.stdout)
        self.assertEqual(p.stdout.strip(), "a")

    def test_complete_block_comment_is_whitespace(self) -> None:
        expected = {"B": "a\nb", "C": "a"}
        for key in ("B", "C"):
            for entry, strict in FACES:
                with self.subTest(case=key, entry=entry, strict=strict):
                    p = _run(SOURCES[key], entry, strict)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    self.assertEqual(p.stdout.strip(), expected[key], p.stdout)
                    self.assertNotIn("invalid character", p.stderr)

    def test_datum_comment_is_rejected(self) -> None:
        for key in ("D", "E"):
            for entry, strict in FACES:
                with self.subTest(case=key, entry=entry, strict=strict):
                    p = _run(SOURCES[key], entry, strict)
                    self.assertNotEqual(p.returncode, 0, p.stdout)
                    self.assertIn("invalid character", p.stderr)
                    self.assertNotIn("x", p.stdout)
        # D is on line 2: the error points there, not at 1:1.
        for entry, strict in FACES:
            with self.subTest(case="D-position", entry=entry, strict=strict):
                p = _run(SOURCES["D"], entry, strict)
                self.assertIn("2:1", p.stderr, p.stderr)

    def test_bare_hash_is_rejected(self) -> None:
        for entry, strict in FACES:
            with self.subTest(entry=entry, strict=strict):
                p = _run(SOURCES["F"], entry, strict)
                self.assertNotEqual(p.returncode, 0, p.stdout)
                self.assertIn("invalid character", p.stderr)
                self.assertIn("2:1", p.stderr, p.stderr)
                # Neither the bare # nor the form after it is a value form.
                self.assertNotIn("b", p.stdout)
        p = _run(SOURCES["F"], "direct", "0")
        self.assertEqual(p.stdout.strip(), "a")


if __name__ == "__main__":
    unittest.main()
