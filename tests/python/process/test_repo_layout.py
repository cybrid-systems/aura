#!/usr/bin/env python3
"""Issue #1939 — final tests/ layout cleanup invariants."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TESTS = ROOT / "tests"
MIGRATE = TESTS / "migrate_test_layout.py"


class Layout1939(unittest.TestCase):
    def test_required_category_dirs(self) -> None:
        for name in (
            "python",
            "bench",
            "fuzz",
            "e2e",
            "suite",
            "fixtures",
            "compiler",
            "core",
        ):
            self.assertTrue((TESTS / name).is_dir(), f"missing tests/{name}/")
        for name in ("memory", "domain"):
            self.assertFalse((TESTS / name).exists(), f"tests/{name}/ was removed")

    def test_python_harness_full_drivers(self) -> None:
        for name in (
            "_aura_harness.py",
            "run.py",
            "run_issue_tests.py",
            "issue_tier.py",
            "integ_cases.py",
            "fixture_check.py",
            "fixture_store.py",
            "e2e_harness.py",
            "run_e2e.py",
        ):
            self.assertTrue((TESTS / "python" / name).is_file(), name)

    def test_no_full_python_drivers_at_root(self) -> None:
        forbidden = (
            "_aura_harness.py",
            "issue_tier.py",
            "integ_cases.py",
            "smoke_cases.py",
            "regression_cases.py",
            "fixture_store.py",
            "benchmark_cases.py",
            "benchmark_baseline.json",
            "e2e_harness.py",
        )
        for name in forbidden:
            p = TESTS / name
            self.assertFalse(p.exists(), f"{name} must not live at tests/ root")

    def test_thin_entrypoints_only_scripts(self) -> None:
        for p in sorted(TESTS.glob("*.py")):
            if p.name == "migrate_test_layout.py":
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            self.assertIn(
                "Thin entrypoint",
                text,
                f"{p.name} at tests/ root must be a thin entrypoint (#1939)",
            )
            # Thin shims stay small so nobody sneaks a full driver back in
            self.assertLess(
                len(text.splitlines()),
                40,
                f"{p.name} looks too large for a thin entrypoint",
            )

    def test_migrate_status_clean(self) -> None:
        # tests/migrate_test_layout.py was removed once the layout landed.
        # The live policy is tests/README.md, not a status CLI.
        self.assertFalse(MIGRATE.exists())
        readme = (TESTS / "README.md").read_text(encoding="utf-8")
        self.assertIn("HOMES.md", readme)
        self.assertIn("python/", readme)

    def test_what_changed_docs(self) -> None:
        readme = (TESTS / "README.md").read_text(encoding="utf-8")
        self.assertIn("HOMES.md", readme)
        self.assertIn("python/", readme)
        self.assertIn("e2e/", readme)

    def test_e2e_and_fuzz_present(self) -> None:
        self.assertTrue((TESTS / "e2e" / "README.md").is_file())
        self.assertTrue((TESTS / "fuzz" / "README.md").is_file() or (TESTS / "fuzz").is_dir())
        self.assertTrue((TESTS / "bench" / "benchmark.py").is_file())


PROCESS_THEME = "repo-layout"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ()
CASES = (
    (
        1939,
        "test_required_category_dirs",
        "Layout1939.test_required_category_dirs",
    ),
    (
        1939,
        "test_python_harness_full_drivers",
        "Layout1939.test_python_harness_full_drivers",
    ),
    (
        1939,
        "test_no_full_python_drivers_at_root",
        "Layout1939.test_no_full_python_drivers_at_root",
    ),
    (
        1939,
        "test_thin_entrypoints_only_scripts",
        "Layout1939.test_thin_entrypoints_only_scripts",
    ),
    (
        1939,
        "test_migrate_status_clean",
        "Layout1939.test_migrate_status_clean",
    ),
    (
        1939,
        "test_what_changed_docs",
        "Layout1939.test_what_changed_docs",
    ),
    (
        1939,
        "test_e2e_and_fuzz_present",
        "Layout1939.test_e2e_and_fuzz_present",
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
