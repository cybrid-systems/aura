"""Issue #5411: hot rebind keeps long-running runtime state."""

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


def run(program: str) -> dict[str, str]:
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "probe.aura"
        p.write_text(program)
        env = os.environ.copy()
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "0"
        r = subprocess.run([str(AURA_BIN), str(p)], capture_output=True, text=True, timeout=120, env=env)
        if r.returncode != 0:
            raise AssertionError(f"aura rc={r.returncode}\n{r.stdout}\n{r.stderr}")
        return {k: v for k, v in (line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)}


PROBE = r"""
(require "std/hot-strategy" all:)
(define (p k v) (display k) (display "=") (display v) (newline))
(set-code "(define counter 0)\n(define (tick) (set! counter (+ counter 1)))")
(eval-current)
(tick)
(tick)
(p "before" counter)
(p "rebind" (mutate :rebind "tick"
                    "(lambda () (set! counter (+ counter 10)))" "hot"))
(tick)
(p "after_rebind" counter)
(p "bad" (mutate :rebind "tick" "(lambda () (undefined-call))" "bad"))
(tick)
(p "after_bad" counter)
(p "register" (car (hot-strategy:register! "tick"
                    "(lambda () (set! counter (+ counter 10)))")))
(p "swap" (car (hot-strategy:swap! "tick"
                    "(lambda () (set! counter (+ counter 100)))" "swap")))
(tick)
(p "after_swap" counter)
(p "heal" (hot-strategy:heal!))
(tick)
(p "after_heal" counter)
"""


class HotState(unittest.TestCase):
    def test_rebind_preserves_state(self) -> None:
        out = run(PROBE)
        self.assertEqual(out["before"], "2")
        self.assertEqual(out["rebind"], "#t")
        self.assertEqual(out["after_rebind"], "12")
        self.assertNotEqual(out["bad"], "#t")
        self.assertEqual(out["after_bad"], "22")
        self.assertEqual(out["register"], "#t")
        self.assertEqual(out["swap"], "#t")
        self.assertEqual(out["after_swap"], "122")
        self.assertEqual(out["heal"], "#t")
        self.assertEqual(out["after_heal"], "132")
