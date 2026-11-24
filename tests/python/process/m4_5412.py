"""Issue #5412: mutate:rebind keeps untouched source text byte-identical."""

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


def _run(program: str, count: int) -> tuple[list[str], str]:
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "probe.aura"
        p.write_text(program)
        env = os.environ.copy()
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "0"
        r = subprocess.run([str(AURA_BIN), str(p)], capture_output=True, text=True, timeout=120, env=env)
        if r.returncode != 0:
            raise AssertionError(f"aura rc={r.returncode}\n{r.stdout}\n{r.stderr}")
        blocks: list[str] = []
        pos = 0
        while True:
            try:
                start = r.stdout.index("BEGIN\n", pos) + len("BEGIN\n")
                end = r.stdout.index("END\n", start)
            except ValueError:
                break
            blocks.append(r.stdout[start:end])
            pos = end + len("END\n")
        if len(blocks) != count:
            raise AssertionError(f"expected {count} source blocks, got {len(blocks)}: {r.stdout!r}")
        rest = r.stdout
        for block in blocks:
            rest = rest.replace("BEGIN\n" + block + "END\n", "", 1)
        return blocks, rest


def run(program: str) -> list[str]:
    blocks, _rest = _run(program, 2)
    return blocks


PROBE = r"""
(set-code "; header comment\n(define a 1) ; a stays\n\n(define b 2) ; b changes\n")
(eval-current)
(display "BEGIN\n")
(display (current-source :workspace :preserve))
(display "END\n")
(mutate :rebind "b" "3" "change b")
(display "BEGIN\n")
(display (current-source :workspace :preserve))
(display "END\n")
"""


class SourceFidelity(unittest.TestCase):
    def test_untouched_comments_and_layout_survive(self) -> None:
        before, after = run(PROBE)
        self.assertEqual(before, "; header comment\n(define a 1) ; a stays\n\n(define b 2) ; b changes\n")
        self.assertEqual(after, "; header comment\n(define a 1) ; a stays\n\n(define b 3) ; b changes\n")


# Issue #5438: a rebind that changed the line count moved every define after
# it, and the next rebind patched the text at the stale offset: the export
# lost b's body and carried a define inside a call and a stale c.
LINE_SHIFT = r"""
(set-code "(define a 1)\n(define b (+ 1 2))\n(define c 3)\n")
(eval-current)
(mutate :rebind "b" "(+ 10\n (- 5 3))" "b grows by a line")
(mutate :rebind "c" "4" "c follows it")
(display "BEGIN\n")
(display (current-source :workspace :preserve))
(display "END\n")
(display "values ") (display (list a b c)) (newline)
; the export must read back to the same program
(set-code (current-source :workspace :preserve))
(eval-current)
(display "reload ") (display (list a b c)) (newline)
"""

# Issue #5438: the closing paren of the canonical wrapper landed inside the
# comment, and the published source did not read back.
TRAILING_COMMENT = r"""
(set-code "(define (f x) (+ x 1))\n")
(eval-current)
(mutate :rebind "f" "(lambda (x) (* x 2)) ; doubled" "x")
(display "BEGIN\n")
(display (current-source :workspace :preserve))
(display "END\n")
(set-code (current-source :workspace :preserve))
(eval-current)
(display "reload ") (display (f 3)) (newline)
"""


class RebindSourcePatch5438(unittest.TestCase):
    def test_second_rebind_after_a_line_count_change(self) -> None:
        (block,), rest = _run(LINE_SHIFT, 1)
        self.assertEqual(block, "(define a 1)\n(define b (+ 10\n (- 5 3)))\n(define c 4)\n")
        self.assertIn("values (1 12 4)\n", rest)
        self.assertIn("reload (1 12 4)\n", rest)

    def test_body_ending_in_a_line_comment_reads_back(self) -> None:
        (block,), rest = _run(TRAILING_COMMENT, 1)
        self.assertEqual(block, "(define f (lambda (x) (* x 2)) ; doubled\n)\n")
        self.assertIn("reload 6\n", rest)
