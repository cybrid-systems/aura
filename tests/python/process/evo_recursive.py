#!/usr/bin/env python3
r"""Regression for #5052: a recursive definition must be installable at runtime.

Before: the recursive call's result type was inferred as Any (the callee is a
not-yet-unified letrec forward variable) and the production join gate then
rejected the resulting `Int ~ Any` pair as a HARD type error. Under the
production defaults every `mutate:rebind` of a recursive definition was
refused with "solve_delta not SOLVED (production fail-closed)" — the same
source loaded from a file passed, so the evolution loop could never keep a
recursive candidate.

The test drives the real CLI on the production-default face (AURA_SANDBOX is
NOT set; `--preset dev` only adds the Mutate grant).
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

# A recursive definition: the loop's return type is only pinned by the
# recursive call itself, so the inference has to keep it a variable.
RECURSIVE = "(define (mystery s) (let loop ((i 0) (acc 0)) (if (= i (string-length s)) acc (loop (+ i 1) (+ acc 1)))))"

PROGRAM = f"""
(set-code "(define (mystery s) 0)")
(eval-current)
(display (mutate :rebind "mystery" "{RECURSIVE}" "5052"))
(newline)
(display (mystery "abcd"))
(newline)
"""


def run(program: str, extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    # Production defaults: no AURA_SANDBOX, no AURA_PRESET (the flag adds the
    # Mutate grant only).
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
        env.pop(k, None)
    env.update(extra_env or {})
    with tempfile.TemporaryDirectory() as tmp:
        f = Path(tmp) / "rebind_recursive.aura"
        f.write_text(program)
        return subprocess.run(
            [str(AURA_BIN), "--preset", "dev", str(f)],
            cwd=tmp,
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
            stdin=subprocess.DEVNULL,
        )


class RecursiveRebind(unittest.TestCase):
    def test_recursive_rebind_is_accepted(self) -> None:
        r = run(PROGRAM)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn(
            "solve_delta not SOLVED",
            r.stdout + r.stderr,
            "the recursive rebind must not be fail-closed",
        )
        lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
        self.assertEqual(lines[0], "#t", "the recursive rebind must be accepted")
        self.assertEqual(lines[1], "4", "the installed recursive body must run")

    def test_non_recursive_rebind_still_accepted(self) -> None:
        r = run(
            """
(set-code "(define (mystery s) 0)")
(eval-current)
(display (mutate :rebind "mystery" "(define (mystery s) (list 1 2 3))" "5052b"))
(newline)
"""
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("#t", r.stdout)


if __name__ == "__main__":
    unittest.main()
