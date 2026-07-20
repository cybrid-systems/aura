#!/usr/bin/env python3
"""Issue #1964 — Phase 2 architectural simplification surface checks."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "src" / "core"
COMPILER = ROOT / "src" / "compiler"
DOCS = ROOT / "docs"


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
        self.assertIn("WorkspaceEpoch", we)
        self.assertIn("current_mutation_epoch", we)
        self.assertIn("current_bridge_epoch", we)
        self.assertIn("TransactionGuard", tg)
        self.assertIn("cycle 3", tg)
        self.assertIn("mutate_dispatch", md)
        self.assertIn("MutateKind", md)
        self.assertIn("cycle 4", md)

    def test_set_body_bookkeeps_dispatch(self) -> None:
        mut = (COMPILER / "evaluator_primitives_mutate.cpp").read_text(encoding="utf-8")
        self.assertIn("mutate_dispatch(MutateKind::SetBody", mut)
        self.assertIn("TransactionGuard", mut)
        self.assertIn("query:architectural-simplification-stats", mut)

    def test_docs_reflect_shipped_cycles(self) -> None:
        doc = (DOCS / "agent-safety-mechanisms-simplification.md").read_text(encoding="utf-8")
        self.assertIn("cycle 4", doc.lower())
        self.assertIn("TransactionGuard", doc)
        self.assertIn("mutate_dispatch", doc)
        self.assertIn("surfaces shipped", doc)
        self.assertIn("architectural-simplification-stats", doc)

    def test_epoch_migration_linter_exists(self) -> None:
        script = ROOT / "scripts" / "check_workspace_epoch_migration.py"
        self.assertTrue(script.is_file())


if __name__ == "__main__":
    unittest.main()
