#!/usr/bin/env python3
"""Issue #5235: arity/location and CLI-face diagnostics."""

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


def run(args: list[str], cwd: str | None = None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_IR_PIPELINE_TRACE", None)
    return subprocess.run([str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120)


class Diag5235(unittest.TestCase):
    def test_if_requires_branches(self) -> None:
        r = run(["-e", "(if)"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("(if cond then [else])", r.stderr)
        self.assertIn("1:4", r.stderr)

    def test_define_missing_name_has_location(self) -> None:
        r = run(["-e", "(define)"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("requires a name", r.stderr)
        self.assertIn("1:8", r.stderr)

    def test_jit_reads_file_argument(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "p.aura"
            p.write_text("(display (car 5))\n")
            r = run(["--jit", str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("car: not a pair", r.stderr)
            self.assertNotIn("unbound variable", r.stderr)

    def test_short_name_gets_no_noise_suggestion(self) -> None:
        r = run(["-e", "(baz)"])
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn("did you mean", r.stderr)

    def test_memq_keeps_its_useful_hint(self) -> None:
        r = run(["-e", "(memq 1 (list 1))"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("member", r.stderr)

    def test_ir_error_exits_nonzero_without_pipeline_trace(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "p.aura"
            p.write_text("(display (car 5))\n")
            r = run(["--ir", str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("car: not a pair", r.stderr)
            self.assertNotIn("PM: running", r.stderr)


if __name__ == "__main__":
    unittest.main()
