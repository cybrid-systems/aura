#!/usr/bin/env python3
"""Issue #1939 — final tests/ layout cleanup invariants."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"
MIGRATE = TESTS / "migrate_test_layout.py"


class Layout1939(unittest.TestCase):
    def test_required_category_dirs(self) -> None:
        for name in (
            "python",
            "bench",
            "fuzz",
            "memory",
            "e2e",
            "domain",
            "suite",
            "fixtures",
        ):
            self.assertTrue((TESTS / name).is_dir(), f"missing tests/{name}/")

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
        r = subprocess.run(
            [sys.executable, str(MIGRATE), "--status"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            r.returncode,
            0,
            f"--status failed:\n{r.stdout}\n{r.stderr}",
        )
        self.assertIn("layout policy clean", r.stdout)

    def test_what_changed_docs(self) -> None:
        readme = (TESTS / "README.md").read_text(encoding="utf-8")
        self.assertIn("What changed", readme)
        self.assertIn("1939", readme)
        self.assertIn("python/", readme)
        harness = (ROOT / "docs" / "test_harness_pattern.md").read_text(encoding="utf-8")
        self.assertIn("1939", harness)
        self.assertIn("e2e/", harness)

    def test_e2e_and_fuzz_present(self) -> None:
        self.assertTrue((TESTS / "e2e" / "README.md").is_file())
        self.assertTrue((TESTS / "fuzz" / "README.md").is_file() or (TESTS / "fuzz").is_dir())
        self.assertTrue((TESTS / "bench" / "benchmark.py").is_file())


if __name__ == "__main__":
    unittest.main()
