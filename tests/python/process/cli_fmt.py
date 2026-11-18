#!/usr/bin/env python3
"""Process checks for the aura --fmt face.

The reader keeps no trivia: Lexer::skip_ws drops ';' and '#| |#' and the AST
has no spans. So --fmt used to answer by unparsing, which deleted every
comment (Chinese ones included), desugared (define (f x) ...) into
(define f (lambda ...)), wrapped multi-form files in (begin ...), and still
exited 0 while doing it in place.

First wave of the fix: -i and --check refuse comment-bearing input (nonzero
exit, nothing written); a stdout run warns. Second wave: the conservative
reindent, which keeps every comment and literal byte. These checks run the
real binary and compare file bytes, not just exit codes.
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

# One line comment (Chinese), one trailing comment, and the define sugar the
# unparser would rewrite.
COMMENTED = "; 保留这一行注释\n(define (f x) (+ x 1)) ; 行尾注释\n(display (f 41))\n"
# ';' inside a string, a character literal, and a |symbol| is not a comment.
NOT_COMMENTS = '(display "; not a comment")\n(display #\\;)\n(display |a;b|)\n'
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


class Fmt(unittest.TestCase):
    def test_in_place_refuses_a_comment(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "commented.aura"
            path.write_text(COMMENTED, encoding="utf-8")
            before = path.read_bytes()
            res = run_aura(["--fmt", "-i", "commented.aura"], cwd=root)
            self.assertNotEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("refus", res.stderr)
            self.assertIn("comment", res.stderr)
            self.assertEqual(path.read_bytes(), before)

    def test_check_refuses_a_comment(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "commented.aura").write_text(COMMENTED, encoding="utf-8")
            res = run_aura(["--fmt", "--check", "commented.aura"], cwd=root)
            self.assertNotEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertNotIn("formatted OK", res.stderr)
            self.assertNotIn("needs formatting", res.stderr)
            self.assertIn("refus", res.stderr)

    def test_stdout_run_warns(self) -> None:
        # One line: this wave still reads stdin with getline (the whole-program
        # read lands with the reindent).
        res = run_aura(["--fmt"], text="(display 1) ; 中文\n")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("warning", res.stderr)
        self.assertIn("comment", res.stderr)
        # The warning is a warning: the (old) output still goes to stdout.
        self.assertIn("(display 1)", res.stdout)

    def test_literals_are_not_comments(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "literal.aura"
            path.write_text(NOT_COMMENTS, encoding="utf-8")
            res = run_aura(["--fmt", "--check", "literal.aura"], cwd=root)
            self.assertNotIn("refus", res.stderr)
            self.assertIn(res.returncode, (0, 1), res.stderr)

    def test_plain_input_still_formats(self) -> None:
        res = run_aura(["--fmt"], text=PLAIN)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertNotIn("warning", res.stderr)
        self.assertIn("(+ x 1)", res.stdout)

    def test_help_names_the_refusal(self) -> None:
        res = run_aura(["help", "--all"])
        blob = res.stdout + res.stderr
        self.assertEqual(res.returncode, 0, blob)
        self.assertIn("--fmt", blob)
        self.assertIn("comment", blob)


if __name__ == "__main__":
    unittest.main()
