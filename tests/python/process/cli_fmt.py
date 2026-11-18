#!/usr/bin/env python3
"""Process checks for the aura --fmt face.

The reader keeps no trivia: Lexer::skip_ws drops ';' and '#| |#' and the AST
has no spans. So --fmt used to answer by unparsing, which deleted every
comment (Chinese ones included), desugared (define (f x) ...) into
(define f (lambda ...)), wrapped multi-form files in (begin ...), and still
exited 0 while doing it in place.

--fmt is now a conservative reindent over the raw source: line-leading
whitespace only, every other byte copied. --unparse keeps the old AST
printer. These checks run the real binary and compare file bytes, and they
run the program before and after to prove the meaning did not move.
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

# Badly indented on purpose. One line comment, one trailing comment, one
# two-line block comment with Chinese text, the define sugar, a lambda the
# source itself wrote, and several top-level forms.
MESSY = """; 文件头注释：保留
(define (f x)
(+ x
1)) ; 行尾注释
#| 块注释
   跨行：保留 |#
(define g (lambda (y) (* y 2)))
(display (f 41))(display "\n")
(display (g 21))(display "\n")
"""
COMMENT_LINES = ("; 文件头注释：保留", "; 行尾注释", "#| 块注释", "跨行：保留 |#")
# ';' inside a string, a character literal, and a |symbol| is not a comment.
LITERALS = '(display "; not a comment")\n(display #\\;)\n(display |a;b|)\n'
BROKEN = "(define (f x)\n  (+ x 1)\n"  # unclosed: the reader rejects it
PLAIN = "(define (f x) (+ x 1))\n(display (f 41))\n"


def run_aura(
    args: list[str],
    text: str = "",
    cwd: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(key, None)
    env["AURA_RUNTIME_DIR"] = str(ROOT)

    def invoke(work: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(AURA_BIN), *args],
            input=text,
            cwd=work,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    if cwd is not None:
        return invoke(cwd)
    with tempfile.TemporaryDirectory() as tmp:
        return invoke(Path(tmp))


def skeleton(text: str) -> str:
    """Every line with its leading blanks removed: what --fmt may not change."""
    return "\n".join(line.lstrip(" \t") for line in text.split("\n"))


class Fmt(unittest.TestCase):
    def test_format_keeps_comments_and_sugar(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "messy.aura"
            path.write_text(MESSY, encoding="utf-8")
            before = run_aura(["messy.aura"], cwd=root)
            self.assertEqual(before.returncode, 0, before.stdout + before.stderr)

            res = run_aura(["--fmt", "-i", "messy.aura"], cwd=root)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            formatted = path.read_text(encoding="utf-8")

            for line in COMMENT_LINES:
                self.assertIn(line, formatted)
            self.assertIn("(define (f x)", formatted)
            self.assertNotIn("(define f (lambda", formatted)
            self.assertNotIn("(begin", formatted)
            self.assertEqual(skeleton(formatted), skeleton(MESSY))
            # Reindenting happened: the body of f moved two columns in.
            self.assertIn("\n  (+ x\n", formatted)

            after = run_aura(["messy.aura"], cwd=root)
            self.assertEqual(after.returncode, 0, after.stdout + after.stderr)
            self.assertEqual(after.stdout, before.stdout)

    def test_format_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "messy.aura"
            path.write_text(MESSY, encoding="utf-8")
            first = run_aura(["--fmt", "-i", "messy.aura"], cwd=root)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            once = path.read_bytes()
            second = run_aura(["--fmt", "-i", "messy.aura"], cwd=root)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertEqual(path.read_bytes(), once)

    def test_check_tracks_the_reindent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "messy.aura").write_text(MESSY, encoding="utf-8")
            (root / "plain.aura").write_text(PLAIN, encoding="utf-8")
            dirty = run_aura(["--fmt", "--check", "messy.aura"], cwd=root)
            self.assertEqual(dirty.returncode, 1, dirty.stderr)
            self.assertIn("needs formatting", dirty.stderr)
            clean = run_aura(["--fmt", "--check", "plain.aura"], cwd=root)
            self.assertEqual(clean.returncode, 0, clean.stderr)
            self.assertIn("formatted OK", clean.stderr)

    def test_unreadable_input_is_not_rewritten(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "broken.aura"
            path.write_text(BROKEN, encoding="utf-8")
            before = path.read_bytes()
            res = run_aura(["--fmt", "-i", "broken.aura"], cwd=root)
            self.assertNotEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("parse error", res.stderr)
            self.assertEqual(path.read_bytes(), before)

    def test_literals_are_not_comments(self) -> None:
        # A ';' in a string, a character literal or a |symbol| must not be
        # mistaken for a comment by the scanner (it would be stripped or the
        # file refused).
        res = run_aura(["--fmt"], text=LITERALS)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn('"; not a comment"', res.stdout)
        self.assertIn("#\\;", res.stdout)
        self.assertIn("|a;b|", res.stdout)

    def test_unparse_keeps_the_ast_contract(self) -> None:
        # --unparse is the AST printer: it still desugars and drops comments.
        # That is its documented job, and it is why --fmt stopped using it.
        res = run_aura(["--unparse"], text=MESSY)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertNotIn("; 文件头注释", res.stdout)
        self.assertIn("(lambda", res.stdout)

    def test_help_names_the_split(self) -> None:
        # The tool list with the fmt wording is in the long help.
        res = run_aura(["help", "--all"])
        blob = res.stdout + res.stderr
        self.assertEqual(res.returncode, 0, blob)
        self.assertIn("--fmt", blob)
        self.assertIn("reindent", blob)
        self.assertIn("--unparse", blob)


if __name__ == "__main__":
    unittest.main()
