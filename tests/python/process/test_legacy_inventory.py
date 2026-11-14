#!/usr/bin/env python3
"""Issue #1957 — legacy test inventory living-doc invariants."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "tools" / "inventory_legacy_tests.py"
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


PROCESS_THEME = "legacy-inventory"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ()
CASES = (
    (
        1957,
        "test_script_and_inventory_exist",
        "Inventory1957.test_script_and_inventory_exist",
    ),
    (
        1957,
        "test_inventory_cites_1957_and_themes",
        "Inventory1957.test_inventory_cites_1957_and_themes",
    ),
    (
        1957,
        "test_check_mode_clean",
        "Inventory1957.test_check_mode_clean",
    ),
    (
        1957,
        "test_no_new_test_issue_policy_in_readme",
        "Inventory1957.test_no_new_test_issue_policy_in_readme",
    ),
)


def load_tests(loader, tests, pattern):
    import importlib
    import sys

    suite = unittest.TestSuite()
    for _issue, _title, spec in CASES:
        bits = spec.split(".")
        if len(bits) == 3:
            mod = importlib.import_module(bits[0])
            suite.addTest(getattr(mod, bits[1])(bits[2]))
        else:
            suite.addTest(getattr(sys.modules[__name__], bits[0])(bits[1]))
    return suite


if __name__ == "__main__":
    unittest.main()
