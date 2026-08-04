#!/usr/bin/env python3
"""Issue #1964 / #2039 — Phase 2 architectural simplification surface checks."""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "src" / "core"
COMPILER = ROOT / "src" / "compiler"
SCRIPTS = ROOT / "scripts"


class ArchitecturalSimplification1964(unittest.TestCase):
    def test_headers_exist(self) -> None:
        self.assertTrue((CORE / "workspace_epoch.hh").is_file())
        self.assertTrue((CORE / "transaction_guard.hh").is_file())
        self.assertTrue((COMPILER / "mutate_dispatch.hh").is_file())
        self.assertFalse(
            (CORE / "provenance_tracker.ixx").exists(),
            "cycle 1 deleted dual-track provenance module",
        )

    def test_header_cites_cycles(self) -> None:
        we = (CORE / "workspace_epoch.hh").read_text(encoding="utf-8")
        tg = (CORE / "transaction_guard.hh").read_text(encoding="utf-8")
        md = (COMPILER / "mutate_dispatch.hh").read_text(encoding="utf-8")
        self.assertIn("1964", we)
        self.assertIn("2039", we)
        self.assertIn("WorkspaceEpoch", we)
        self.assertIn("current_mutation_epoch", we)
        self.assertIn("current_bridge_epoch", we)
        self.assertIn("bump_mutation_and_bridge_epochs", we)
        self.assertIn("Final ownership model", we)
        self.assertIn("TransactionGuard", tg)
        self.assertIn("cycle 3", tg)
        # Issue #2555: real host path (scaffold simulation removed).
        self.assertIn("2555", tg)
        self.assertIn("TransactionGuardHost", tg)
        self.assertNotIn("simulate boundary acquisition", tg)
        self.assertIn("mutate_dispatch", md)
        self.assertIn("MutateKind", md)
        self.assertIn("cycle 4", md)

    def test_set_body_bookkeeps_dispatch(self) -> None:
        mut = (COMPILER / "evaluator_primitives_mutate.cpp").read_text(encoding="utf-8")
        self.assertIn("mutate_dispatch(MutateKind::SetBody", mut)
        self.assertIn("TransactionGuard", mut)
        self.assertIn("query:architectural-simplification-stats", mut)

    def test_mutation_epoch_field_deleted(self) -> None:
        """Issue #2039: CompilerService must not dual-store mutation_epoch_."""
        service = (COMPILER / "service.ixx").read_text(encoding="utf-8")
        # Strip comments roughly for the field-decl check.
        no_line_comments = re.sub(r"//.*?$", "", service, flags=re.M)
        self.assertIsNone(
            re.search(r"\bstd::atomic\s*<[^>]+>\s*mutation_epoch_", no_line_comments),
            "legacy CompilerService::mutation_epoch_ field must be deleted",
        )
        self.assertIsNone(
            re.search(r"\bmutation_epoch_\.(?:load|store|fetch_add)\b", no_line_comments),
            "no mutation_epoch_ field ops remain",
        )
        self.assertIn("aura::core::current_mutation_epoch()", service)
        self.assertIn("aura::core::bump_mutation_and_bridge_epochs()", service)

    def test_bridge_dual_write_wired(self) -> None:
        """Issue #2039: C aura_set dual-writes WorkspaceEpoch::Bridge."""
        bridge = (COMPILER / "aura_jit_bridge.cpp").read_text(encoding="utf-8")
        self.assertIn("workspace_epoch.hh", bridge)
        self.assertIn("store_workspace_epoch", bridge)
        self.assertIn("WorkspaceEpochKind::Bridge", bridge)
        service = (COMPILER / "service.ixx").read_text(encoding="utf-8")
        self.assertIn("aura_set_current_bridge_epoch", service)
        self.assertIn("bump_mutation_and_bridge_epochs", service)

    def test_epoch_migration_linter_strict_clean(self) -> None:
        script = SCRIPTS / "coverage" / "checks" / "check_workspace_epoch_migration.py"
        self.assertTrue(script.is_file())
        r = subprocess.run(
            [sys.executable, str(script), "--strict", "--quiet"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            r.returncode,
            0,
            f"linter --strict must be clean after #2039:\n{r.stdout}\n{r.stderr}",
        )
        self_test = subprocess.run(
            [sys.executable, str(script), "--self-test"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(self_test.returncode, 0, self_test.stdout + self_test.stderr)


if __name__ == "__main__":
    unittest.main()
