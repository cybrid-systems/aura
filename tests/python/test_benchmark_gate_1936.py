#!/usr/bin/env python3
"""Issue #1936 — pure unit tests for statistical/relative benchmark gate."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH_PY = ROOT / "tests" / "bench" / "benchmark.py"


def load_bench():
    sys.path.insert(0, str(ROOT / "tests" / "bench"))
    sys.path.insert(0, str(ROOT / "tests" / "python"))  # fixture_store
    spec = importlib.util.spec_from_file_location("benchmark_mod", BENCH_PY)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules["benchmark_mod"] = mod  # required for @dataclass on 3.14
    spec.loader.exec_module(mod)
    return mod


class BenchmarkGate1936(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.m = load_bench()

    def test_median(self) -> None:
        self.assertAlmostEqual(self.m.median_time([3.0, 1.0, 2.0]), 2.0)
        self.assertAlmostEqual(self.m.median_time([5.0]), 5.0)

    def test_classify_relative_noise_under_floor(self) -> None:
        # 5ms → 9ms is +80% but under 20ms floor → not a regression
        hit = self.m.classify_regression(0.005, 0.009, ratio_threshold=1.05, min_delta_s=0.020)
        self.assertIsNone(hit)

    def test_classify_relative_real_regression(self) -> None:
        # 100ms → 130ms with 5% tol and 20ms floor → regression
        hit = self.m.classify_regression(0.100, 0.130, ratio_threshold=1.05, min_delta_s=0.020)
        self.assertIsNotNone(hit)
        assert hit is not None
        self.assertFalse(hit.catastrophic)
        self.assertGreater(hit.ratio, 1.05)

    def test_classify_catastrophic(self) -> None:
        # 10ms → 40ms is 4× and Δ=30ms > 20ms floor → catastrophic
        hit = self.m.classify_regression(0.010, 0.040, ratio_threshold=1.05, min_delta_s=0.020)
        self.assertIsNotNone(hit)
        assert hit is not None
        self.assertTrue(hit.catastrophic)

    def test_classify_catastrophic_still_needs_floor(self) -> None:
        # 5ms → 18ms is 3.6× but Δ=13ms under 20ms floor → not a regression (#1936)
        hit = self.m.classify_regression(0.005, 0.018, ratio_threshold=1.05, min_delta_s=0.020)
        self.assertIsNone(hit)

    def test_case_thresholds_meta(self) -> None:
        meta = {
            "defaults": {"tolerance_percent": 20, "min_delta_ms": 20},
            "cases": {"literal_int": {"tolerance_percent": 25, "min_delta_ms": 30}},
        }
        thr, md, cat = self.m.case_thresholds(
            "literal_int",
            tolerance_percent=20,
            min_delta_s=0.02,
            catastrophic_ratio=3.0,
            meta=meta,
        )
        self.assertAlmostEqual(thr, 1.25)
        self.assertAlmostEqual(md, 0.03)
        self.assertAlmostEqual(cat, 3.0)

    def test_update_requires_rationale(self) -> None:
        # main() must exit 2 without rationale *before* run_all (#1936 fail-fast)
        m = self.m
        original = m.run_all
        called = {"n": 0}

        def fake_run_all(*, runs: int = 1):
            called["n"] += 1
            return m.BenchSuiteResult(
                timestamp="t",
                total_cases=0,
                passed=0,
                failed=0,
                total_time_s=0.0,
                cases=[],
            )

        m.run_all = fake_run_all  # type: ignore[method-assign]
        try:
            rc = m.main(["--update"])
            self.assertEqual(rc, 2)
            self.assertEqual(called["n"], 0, "run_all must not run without rationale")
        finally:
            m.run_all = original  # type: ignore[method-assign]

    def test_docs_and_meta_exist(self) -> None:
        self.assertTrue((ROOT / "docs" / "benchmark.md").is_file())
        self.assertTrue((ROOT / "tests" / "bench" / "benchmark_meta.json").is_file())
        self.assertTrue((ROOT / "tests" / "bench" / "benchmark_updates.md").is_file())
        doc = (ROOT / "docs" / "benchmark.md").read_text(encoding="utf-8")
        self.assertIn("1936", doc)
        self.assertIn("tolerance", doc)
        self.assertIn("rationale", doc)

    def test_append_update_log(self) -> None:
        m = self.m
        suite = m.BenchSuiteResult(total_cases=1, passed=1, failed=0, total_time_s=0.1, cases=[])
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "updates.md"
            original = m.UPDATES_LOG
            m.UPDATES_LOG = log  # type: ignore[misc]
            try:
                m.append_update_log(rationale="unit test", suite=suite, argv=["x"])
                text = log.read_text(encoding="utf-8")
                self.assertIn("unit test", text)
                self.assertIn("Rationale", text)
            finally:
                m.UPDATES_LOG = original  # type: ignore[misc]


if __name__ == "__main__":
    unittest.main()
