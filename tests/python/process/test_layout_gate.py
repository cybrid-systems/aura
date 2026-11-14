#!/usr/bin/env python3
"""Issue #1932 — tests/ layout invariants (static)."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TESTS = ROOT / "tests"

# tests/memory and tests/domain were removed. The live tree is the one
# tests/README.md describes.
REQUIRED_DIRS = ("python", "bench", "fuzz", "e2e", "suite", "fixtures")

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
        readme = (TESTS / "README.md").read_text(encoding="utf-8")
        self.assertIn("python/", readme)
        self.assertIn("HOMES.md", readme)
        self.assertIn("CMake", readme)
        self.assertFalse((TESTS / "memory").exists())
        self.assertFalse((TESTS / "domain").exists())


PROCESS_THEME = "layout-gate"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ()
CASES = (
    (
        1932,
        "test_required_dirs",
        "Layout1932.test_required_dirs",
    ),
    (
        1932,
        "test_python_harness",
        "Layout1932.test_python_harness",
    ),
    (
        1932,
        "test_bench_drivers",
        "Layout1932.test_bench_drivers",
    ),
    (
        1932,
        "test_thin_entrypoints",
        "Layout1932.test_thin_entrypoints",
    ),
    (
        1932,
        "test_no_full_drivers_at_root",
        "Layout1932.test_no_full_drivers_at_root",
    ),
    (
        1932,
        "test_docs",
        "Layout1932.test_docs",
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
