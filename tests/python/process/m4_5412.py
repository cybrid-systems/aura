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


def run(program: str) -> list[str]:
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "probe.aura"
        p.write_text(program)
        env = os.environ.copy()
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "0"
        r = subprocess.run([str(AURA_BIN), str(p)], capture_output=True, text=True,
                           timeout=120, env=env)
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
        if len(blocks) != 2:
            raise AssertionError(f"expected two source blocks, got {len(blocks)}: {r.stdout!r}")
        return blocks


PROBE = r'''
(set-code "; header comment\n(define a 1) ; a stays\n\n(define b 2) ; b changes\n")
(eval-current)
(display "BEGIN\n")
(display (current-source :workspace :preserve))
(display "END\n")
(mutate :rebind "b" "3" "change b")
(display "BEGIN\n")
(display (current-source :workspace :preserve))
(display "END\n")
'''


class SourceFidelity(unittest.TestCase):
    def test_untouched_comments_and_layout_survive(self) -> None:
        before, after = run(PROBE)
        self.assertEqual(before, "; header comment\n(define a 1) ; a stays\n\n(define b 2) ; b changes\n")
        self.assertEqual(after, "; header comment\n(define a 1) ; a stays\n\n(define b 3) ; b changes\n")
