#!/usr/bin/env python3
"""Unit tests for scripts/run_pets_regression.py (#1454)."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "run_pets_regression.py"


def _load():
    spec = importlib.util.spec_from_file_location("run_pets_regression", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestPetsRegressionHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def test_demos_listed(self):
        names = [Path(d).name for d in self.m.DEMOS]
        self.assertIn("snake.aura", names)
        self.assertIn("tetris.aura", names)
        self.assertIn("cyber_cat.aura", names)

    def test_fatal_re(self):
        self.assertTrue(self.m.FATAL_RE.search("internal error: boom"))
        self.assertTrue(self.m.FATAL_RE.search("AddressSanitizer: heap-use-after-free"))
        self.assertFalse(self.m.FATAL_RE.search("cyber-cat-run returns #t"))

    def test_pets_binaries_include_smokes(self):
        self.assertIn("test_aura_pets_smoke", self.m.PETS_BINARIES)
        self.assertIn("test_cyber_cat_smoke", self.m.PETS_BINARIES)
        self.assertIn("test_terminal_lifecycle", self.m.PETS_BINARIES)

    def test_demo_files_exist(self):
        for rel in self.m.DEMOS:
            self.assertTrue((ROOT / rel).is_file(), rel)


if __name__ == "__main__":
    unittest.main()
