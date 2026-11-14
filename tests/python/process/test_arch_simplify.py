#!/usr/bin/env python3
"""Issue #1964 / #2039 — Phase 2 architectural simplification surface checks."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
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
        # Issue #3074: live dispatch (no cycle-4 simulate). Set-body notes
        # via mutate_dispatch_note; structural bodies use try_acquire wrapper.
        self.assertIn("mutate_dispatch_note(MutateKind::SetBody", mut)
        self.assertIn("mutate_dispatch_try_acquire", mut)
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
        # check_workspace_epoch_migration.py was removed after the bridge
        # landed. The symbols the other methods read are the remaining contract.
        script = SCRIPTS / "coverage" / "checks" / "check_workspace_epoch_migration.py"
        self.assertFalse(script.exists())
        service = (COMPILER / "service.ixx").read_text(encoding="utf-8")
        self.assertIn("bump_mutation_and_bridge_epochs", service)


PROCESS_THEME = "arch-simplify"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ()
CASES = (
    (
        1964,
        "test_headers_exist",
        "ArchitecturalSimplification1964.test_headers_exist",
    ),
    (
        1964,
        "test_header_cites_cycles",
        "ArchitecturalSimplification1964.test_header_cites_cycles",
    ),
    (
        1964,
        "test_set_body_bookkeeps_dispatch",
        "ArchitecturalSimplification1964.test_set_body_bookkeeps_dispatch",
    ),
    (
        1964,
        "test_mutation_epoch_field_deleted",
        "ArchitecturalSimplification1964.test_mutation_epoch_field_deleted",
    ),
    (
        1964,
        "test_bridge_dual_write_wired",
        "ArchitecturalSimplification1964.test_bridge_dual_write_wired",
    ),
    (
        1964,
        "test_epoch_migration_linter_strict_clean",
        "ArchitecturalSimplification1964.test_epoch_migration_linter_strict_clean",
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
