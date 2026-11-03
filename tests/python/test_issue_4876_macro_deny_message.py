#!/usr/bin/env python3
"""Regression for #4876: macro denial must be actionable."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AURA_BIN = ROOT / "build" / "aura"

SOURCE = """
(define-hygienic-macro (or2-h a b) (let ((t a)) (if t t b)))
(display (or2-h #f 3)) (newline)
"""


def _run(sandbox, soft=False, timeout=60.0):
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(SOURCE)
        path = f.name
    env = dict(os.environ)
    env.pop("AURA_SANDBOX", None)
    if sandbox is not None:
        env["AURA_SANDBOX"] = sandbox
    args = [str(AURA_BIN)]
    if soft:
        args.append("soft")
    args += ["--load", path]
    try:
        return subprocess.run(args, capture_output=True, text=True, env=env, timeout=timeout)
    finally:
        os.unlink(path)


class TestMacroDenyMessage4876(unittest.TestCase):
    def test_restricted_names_the_effect_and_escape(self):
        p = _run(None)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("effect-denied: macro-self-evo not granted", p.stderr)
        self.assertIn("AURA_SANDBOX=off", p.stderr)

    def test_off_expands(self):
        p = _run("off")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), "3")


if __name__ == "__main__":
    unittest.main()
