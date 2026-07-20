#!/usr/bin/env python3
"""Issue #1932 — tests/ layout invariants (static)."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"

REQUIRED_DIRS = ("python", "bench", "fuzz", "memory", "domain", "suite", "fixtures")

# Real drivers must not live at tests/ root (thin entrypoints OK).
FORBIDDEN_ROOT_DRIVERS = (
    "_aura_harness.py",
    "issue_tier.py",
    "integ_cases.py",
    "smoke_cases.py",
    "regression_cases.py",
    "fixture_store.py",
    "mutation_loop.py",
    "benchmark_cases.py",
    "benchmark_baseline.json",
)


class Layout1932(unittest.TestCase):
    def test_required_dirs(self) -> None:
        for name in REQUIRED_DIRS:
            self.assertTrue((TESTS / name).is_dir(), f"missing tests/{name}/")

    def test_python_harness(self) -> None:
        for name in (
            "run.py",
            "run_issue_tests.py",
            "_aura_harness.py",
            "fixture_store.py",
            "fixture_check.py",
        ):
            self.assertTrue((TESTS / "python" / name).is_file(), name)

    def test_bench_drivers(self) -> None:
        self.assertTrue((TESTS / "bench" / "benchmark.py").is_file())
        self.assertTrue((TESTS / "bench" / "benchmark_baseline.json").is_file())

    def test_thin_entrypoints(self) -> None:
        for name in ("run.py", "run_issue_tests.py", "fixture_check.py", "benchmark.py"):
            p = TESTS / name
            self.assertTrue(p.is_file(), f"thin entry {name}")
            text = p.read_text(encoding="utf-8")
            self.assertIn("Thin entrypoint", text)
            self.assertIn("1932", text)

    def test_no_full_drivers_at_root(self) -> None:
        for name in FORBIDDEN_ROOT_DRIVERS:
            p = TESTS / name
            if p.is_file():
                text = p.read_text(encoding="utf-8", errors="replace")
                self.assertIn(
                    "Thin entrypoint",
                    text,
                    f"{name} should be moved or be a thin entrypoint",
                )

    def test_docs(self) -> None:
        self.assertTrue((ROOT / "docs" / "test_harness_pattern.md").is_file())
        self.assertTrue((TESTS / "migrate_test_layout.py").is_file())
        readme = (TESTS / "README.md").read_text(encoding="utf-8")
        self.assertIn("python/", readme)
        self.assertIn("1932", readme)


if __name__ == "__main__":
    unittest.main()
