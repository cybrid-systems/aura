#!/usr/bin/env python3
"""Issue #1569: unit tests for hard benchmark SLO gate.

Pure-logic tests (no aura binary) for classify_regression / collect /
check_regression messaging. Run: python3 tests/test_benchmark_gate.py
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path
from unittest import mock

# Import under tests/
sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark as bench  # noqa: E402


class TestClassifyRegression(unittest.TestCase):
    def test_no_regression_within_noise(self):
        # 4ms → 6ms is 1.5× but absolute delta 2ms < 20ms floor
        hit = bench.classify_regression(0.004, 0.006)
        self.assertIsNone(hit)

    def test_ratio_and_delta_triggers(self):
        # 50ms → 100ms = 2× and Δ=50ms > 20ms
        hit = bench.classify_regression(0.050, 0.100)
        self.assertIsNotNone(hit)
        self.assertAlmostEqual(hit.ratio, 2.0)
        self.assertFalse(hit.catastrophic)

    def test_catastrophic_ignores_delta_floor(self):
        # 5ms → 20ms = 4× catastrophic even if delta is only 15ms
        hit = bench.classify_regression(0.005, 0.020, catastrophic_ratio=3.0)
        self.assertIsNotNone(hit)
        self.assertTrue(hit.catastrophic)

    def test_below_ratio_ok(self):
        hit = bench.classify_regression(0.100, 0.110)  # 1.1×
        self.assertIsNone(hit)

    def test_zero_base_skipped(self):
        hit = bench.classify_regression(0.0, 1.0)
        self.assertIsNone(hit)


class TestCollectRegressions(unittest.TestCase):
    def test_injected_regression_detected(self):
        baseline = bench.BenchSuiteResult(
            cases=[
                {"name": "fast", "time_s": 0.05},
                {"name": "orch", "time_s": 0.02},
            ]
        )
        suite = bench.BenchSuiteResult(
            cases=[
                {"name": "fast", "time_s": 0.055},  # 1.1×, no hit
                {"name": "orch", "time_s": 0.40},  # 20× catastrophic
            ]
        )
        regs, imps = bench.collect_regressions(suite, baseline)
        self.assertEqual(len(regs), 1)
        self.assertEqual(regs[0].name, "orch")
        self.assertTrue(regs[0].catastrophic)
        self.assertEqual(imps, [])

    def test_improvement_listed(self):
        baseline = bench.BenchSuiteResult(cases=[{"name": "x", "time_s": 0.10}])
        suite = bench.BenchSuiteResult(cases=[{"name": "x", "time_s": 0.05}])
        regs, imps = bench.collect_regressions(suite, baseline)
        self.assertEqual(regs, [])
        self.assertEqual(len(imps), 1)
        self.assertEqual(imps[0][0], "x")


class TestStrictModeEnv(unittest.TestCase):
    def test_env_enables_strict(self):
        with mock.patch.dict(os.environ, {"AURA_CI_STRICT_BENCH": "1"}):
            self.assertTrue(bench.is_strict_mode(["benchmark.py"]))

    def test_flag_enables_strict(self):
        with mock.patch.dict(os.environ, {"AURA_CI_STRICT_BENCH": "0"}):
            self.assertTrue(bench.is_strict_mode(["benchmark.py", "--strict"]))
            self.assertTrue(bench.is_strict_mode(["benchmark.py", "--check"]))

    def test_default_not_strict(self):
        with mock.patch.dict(os.environ, {"AURA_CI_STRICT_BENCH": "0"}, clear=False):
            # Clear might remove other env; force 0
            os.environ["AURA_CI_STRICT_BENCH"] = "0"
            self.assertFalse(bench.is_strict_mode(["benchmark.py"]))


class TestCheckRegressionGate(unittest.TestCase):
    def test_gate_fails_on_injected_regression(self):
        baseline = bench.BenchSuiteResult(
            total_cases=1,
            cases=[{"name": "literal_int", "time_s": 0.05, "passed": True}],
        )
        suite = bench.BenchSuiteResult(
            total_cases=1,
            cases=[{"name": "literal_int", "time_s": 0.50, "passed": True}],
        )
        # Patch load_baseline + fixture names so validate passes.
        fake_case = type("C", (), {"name": "literal_int"})()
        with (
            mock.patch.object(bench, "load_baseline", return_value=baseline),
            mock.patch.object(bench, "load_benchmark_cases", return_value=[fake_case]),
        ):
            ok = bench.check_regression(suite, strict=True)
            self.assertFalse(ok)

    def test_gate_passes_when_clean(self):
        baseline = bench.BenchSuiteResult(
            total_cases=1,
            cases=[{"name": "literal_int", "time_s": 0.05, "passed": True}],
        )
        suite = bench.BenchSuiteResult(
            total_cases=1,
            cases=[{"name": "literal_int", "time_s": 0.052, "passed": True}],
        )
        fake_case = type("C", (), {"name": "literal_int"})()
        with (
            mock.patch.object(bench, "load_baseline", return_value=baseline),
            mock.patch.object(bench, "load_benchmark_cases", return_value=[fake_case]),
        ):
            ok = bench.check_regression(suite, strict=True)
            self.assertTrue(ok)


if __name__ == "__main__":
    unittest.main()
