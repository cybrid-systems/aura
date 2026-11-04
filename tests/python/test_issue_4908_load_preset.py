#!/usr/bin/env python3
"""Regression for #4908 / #4912: plain `aura --load f` honours the preset.

Before: `aura f` read `preset = "dev"` from the aura.toml next to f (or
AURA_PRESET) and granted mutate, but `aura --load f` never resolved a preset,
so the same program was denied in the Restricted default (the starter-kit
mode matrix showed every `--load` cell red). Now a plain `--load f` with a
resolvable preset runs through the file face, like `--preset dev --load f`.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

PROG = '(define r (list 1))\n(set-car! r 2)\n(display (list "v" (car r))) (newline)\n'


class LoadPreset(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.cwd = Path(self.dir.name)
        (self.cwd / "kit").mkdir()
        (self.cwd / "kit" / "p.aura").write_text(PROG)

    def tearDown(self) -> None:
        self.dir.cleanup()

    def run_aura(self, *args: str, **extra: str) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PIPELINE_STRICT"):
            env.pop(k, None)
        env.update(extra)
        return subprocess.run(
            [str(AURA_BIN), *args],
            cwd=self.cwd,
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )

    def check_both_faces_agree(self, **extra: str) -> None:
        direct = self.run_aura("kit/p.aura", **extra)
        load = self.run_aura("--load", "kit/p.aura", **extra)
        self.assertEqual(direct.returncode, 0, direct.stderr)
        self.assertIn("(v 2)", direct.stdout)
        self.assertEqual(load.returncode, 0, load.stderr)
        self.assertIn("(v 2)", load.stdout)
        self.assertNotIn("mutate not granted", load.stderr)

    def test_toml_next_to_entry(self) -> None:
        (self.cwd / "kit" / "aura.toml").write_text('preset = "dev"\n')
        self.check_both_faces_agree()

    def test_toml_next_to_entry_force_soa(self) -> None:
        (self.cwd / "kit" / "aura.toml").write_text('preset = "dev"\n')
        self.check_both_faces_agree(AURA_PIPELINE_STRICT="force-soa")

    def test_env_preset(self) -> None:
        self.check_both_faces_agree(AURA_PRESET="dev")

    def test_no_preset_still_denied(self) -> None:
        r = self.run_aura("--load", "kit/p.aura")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("mutate not granted", r.stderr)


if __name__ == "__main__":
    unittest.main()
