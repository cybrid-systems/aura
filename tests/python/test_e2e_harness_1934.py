#!/usr/bin/env python3
"""Issue #1934 — e2e harness unit tests (no full aura suite)."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from e2e_harness import (  # noqa: E402
    AuraRunResult,
    E2EAssertionError,
    check_e2e_pass,
    check_golden,
    check_pass_labels,
    discover_commercial_readiness,
    golden_for,
)


def _result(**kw) -> AuraRunResult:
    base = dict(
        path=Path("x.aura"),
        exit_code=0,
        stdout="",
        stderr="",
        elapsed_s=0.1,
        pass_labels=[],
        fail_labels=[],
        has_e2e_pass=False,
        has_e2e_fail=False,
        crashed=False,
    )
    base.update(kw)
    return AuraRunResult(**base)


class E2EHarness1934(unittest.TestCase):
    def test_discover_has_at_least_10(self) -> None:
        scripts = discover_commercial_readiness()
        self.assertGreaterEqual(len(scripts), 10)
        for p in scripts:
            self.assertTrue(p.name.startswith("commercial_readiness_"))

    def test_check_e2e_pass_ok(self) -> None:
        r = _result(
            stdout="PASS: a\nPASS: b\nE2E-PASS\n",
            pass_labels=["a", "b"],
            has_e2e_pass=True,
        )
        check_e2e_pass(r, min_passes=2)

    def test_check_e2e_pass_fail_line(self) -> None:
        r = _result(
            stdout="FAIL: a expected=1 actual=2\nE2E-FAIL\n",
            fail_labels=["a expected=1 actual=2"],
            has_e2e_fail=True,
        )
        with self.assertRaises(E2EAssertionError) as cm:
            check_e2e_pass(r)
        self.assertIn("FAIL", str(cm.exception))

    def test_check_e2e_pass_crash(self) -> None:
        r = _result(crashed=True, stderr="AURA CRASH SIGSEGV")
        with self.assertRaises(E2EAssertionError) as cm:
            check_e2e_pass(r)
        self.assertIn("crash", str(cm.exception).lower())

    def test_pass_labels_mismatch_message(self) -> None:
        r = _result(pass_labels=["a", "b"], has_e2e_pass=True, stdout="E2E-PASS")
        with self.assertRaises(E2EAssertionError) as cm:
            check_pass_labels(r, ["a", "c"])
        msg = str(cm.exception)
        self.assertIn("expected", msg)
        self.assertIn("actual", msg)

    def test_golden_roundtrip(self) -> None:
        r = _result(
            path=Path("commercial_readiness_core.aura"),
            pass_labels=["add", "mul"],
            has_e2e_pass=True,
            stdout="PASS: add\nPASS: mul\nE2E-PASS\n",
        )
        with tempfile.TemporaryDirectory() as td:
            g = Path(td) / "all.json"
            g.write_text(
                json.dumps(
                    {
                        "schema": 1934,
                        "issue": 1934,
                        "suites": {
                            "commercial_readiness_core": {
                                "source": "tests/e2e/commercial_readiness/commercial_readiness_core.aura",
                                "pass_labels": ["add", "mul"],
                                "min_passes": 2,
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            check_golden(r, g)  # auto-derives suite_name from r.path.stem

    def test_goldens_exist_for_suite(self) -> None:
        scripts = discover_commercial_readiness()
        self.assertGreater(len(scripts), 0)
        g = golden_for(scripts[0])
        self.assertTrue(g.is_file(), f"missing golden {g}")
        data = json.loads(g.read_text(encoding="utf-8"))
        suites = data.get("suites") or {}
        for script in scripts:
            self.assertIn(script.stem, suites, f"no golden entry for {script.name}")
            entry = suites[script.stem]
            self.assertIn("pass_labels", entry)
            self.assertGreaterEqual(len(entry["pass_labels"]), 1)

    def test_docs(self) -> None:
        readme = (ROOT / "tests" / "e2e" / "README.md").read_text(encoding="utf-8")
        self.assertIn("1934", readme)
        harness = (ROOT / "docs" / "test_harness_pattern.md").read_text(encoding="utf-8")
        self.assertIn("e2e", harness.lower())


if __name__ == "__main__":
    unittest.main()
