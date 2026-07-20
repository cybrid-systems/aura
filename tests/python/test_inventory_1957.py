#!/usr/bin/env python3
"""Issue #1957 — legacy test inventory living-doc invariants."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "inventory_legacy_tests.py"
INVENTORY = ROOT / "tests" / "legacy_test_inventory.md"


class Inventory1957(unittest.TestCase):
    def test_script_and_inventory_exist(self) -> None:
        self.assertTrue(SCRIPT.is_file(), "inventory script")
        self.assertTrue(INVENTORY.is_file(), "inventory markdown")

    def test_inventory_cites_1957_and_themes(self) -> None:
        text = INVENTORY.read_text(encoding="utf-8")
        self.assertIn("1957", text)
        for theme in (
            "arena_compaction",
            "mutation_dirty",
            "fiber_orch",
            "linear_ownership",
            "edsl_hygiene",
            "jit_incremental",
            "shape_soa",
            "observability",
        ):
            self.assertIn(theme, text, theme)
        self.assertIn("Migration priority roadmap", text)
        self.assertIn("Wave", text)

    def test_check_mode_clean(self) -> None:
        r = subprocess.run(
            [sys.executable, str(SCRIPT), "--check"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(r.returncode, 0, f"--check failed:\n{r.stdout}\n{r.stderr}")
        self.assertIn("CHECK OK", r.stdout)

    def test_no_new_test_issue_policy_in_readme(self) -> None:
        readme = (ROOT / "tests" / "README.md").read_text(encoding="utf-8")
        self.assertIn("1957", readme)
        self.assertIn("legacy_test_inventory.md", readme)
        # Policy: do not add new per-issue binaries (various phrasings in README).
        self.assertTrue(
            "do not add new" in readme.lower() or "don't add new" in readme.lower(),
            "README must discourage new test_issue_*.cpp",
        )


if __name__ == "__main__":
    unittest.main()
