#!/usr/bin/env python3
"""Regression for #4912: capability presets, --grant/--deny, a caps banner and --quiet.

Before: a self-evolving program (mutate :rebind + write-file) needed
`aura soft --allow write` or AURA_SANDBOX=off, and nothing said which
capabilities a run had. Now `--preset dev|ci|prod` (or AURA_PRESET, or
`preset = "dev"` in aura.toml) picks a named set, --grant/--deny adjust it,
one stderr line names the effective caps, and --quiet suppresses it.
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

EVO = (
    '(set-code "(define params (quote ((fast . 2))))")\n(eval-current)\n'
    '(display (list "rb" (mutate :rebind "params" "(quote ((fast . 3)))" "x"))) (newline)\n'
    '(eval-current)\n(display (list "v" params)) (newline)\n'
    '(display (list "w" (write-file "out.txt" "hi"))) (newline)\n'
)


class Presets(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.cwd = Path(self.dir.name)
        (self.cwd / "evo.aura").write_text(EVO)

    def tearDown(self) -> None:
        self.dir.cleanup()

    def run_aura(self, *args: str, **extra: str) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING"):
            env.pop(k, None)
        env.update(extra)
        return subprocess.run(
            [str(AURA_BIN), *args, "evo.aura"],
            cwd=self.cwd,
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )

    def test_default_denies_with_hint(self) -> None:
        r = self.run_aura()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("mutate not granted", r.stderr)
        self.assertIn("--preset dev", r.stderr)

    def test_dev_runs_self_evolving_program(self) -> None:
        r = self.run_aura("--preset", "dev")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("(v ((fast . 3)))", r.stdout)
        self.assertIn("(w 1)", r.stdout)
        self.assertEqual((self.cwd / "out.txt").read_text(), "hi")
        self.assertIn("preset=dev", r.stderr)
        self.assertIn("caps=mutate,read,write", r.stderr)

    def test_quiet_drops_banner(self) -> None:
        r = self.run_aura("--preset=dev", "--quiet")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("caps=", r.stderr)

    def test_env_and_toml(self) -> None:
        r = self.run_aura(AURA_PRESET="dev")
        self.assertEqual(r.returncode, 0, r.stderr)
        (self.cwd / "aura.toml").write_text('preset = "dev"\n')
        r = self.run_aura()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("preset=dev", r.stderr)

    def test_grant_and_deny(self) -> None:
        r = self.run_aura("--grant", "mutate")
        self.assertIn("(v ((fast . 3)))", r.stdout, r.stderr)
        self.assertIn("write not granted", r.stdout + r.stderr)
        self.assertIn("caps=mutate,read ", r.stderr)
        r = self.run_aura("--preset", "dev", "--deny", "write")
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((self.cwd / "out.txt").exists())
        r = self.run_aura("--grant", "mutate,write")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_prod_is_default_and_rejects_grant(self) -> None:
        r = self.run_aura("--preset", "prod")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("preset=prod", r.stderr)
        self.assertIn("caps=read", r.stderr)
        r = self.run_aura("--preset", "prod", "--grant", "write")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--preset prod", r.stderr)

    def test_bad_values(self) -> None:
        r = self.run_aura("--preset", "bogus")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("dev | ci | prod", r.stderr)
        r = self.run_aura("--grant", "macro-self-evo")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("never granted from the command line", r.stderr)

    def test_preset_under_sandbox_off(self) -> None:
        r = self.run_aura("--preset", "dev", AURA_SANDBOX="off")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("sandbox=off", r.stderr)


if __name__ == "__main__":
    unittest.main()
