#!/usr/bin/env python3
"""Issue #5637: alloc bench gate logic, wiring, and empty x86 baseline."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "bench"))

import alloc_bench  # noqa: E402

PROCESS_THEME = "alloc-bench"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ()
CASES = (
    (5637, "synthetic ceilings fail by name", "AllocBenchGate.test_high_rss_names_the_metric"),
    (5637, "in-range sample passes", "AllocBenchGate.test_within_ceiling_passes"),
    (5637, "empty platform is record-only", "AllocBenchGate.test_empty_platform_is_record_only"),
    (5637, "readme and nightly wire the bench", "AllocBenchGate.test_readme_and_nightly"),
    (5637, "x86 baseline stays empty", "AllocBenchGate.test_x86_baseline_empty"),
    (5637, "s2 ceiling is the coarse catch", "AllocBenchGate.test_s2_ceiling_is_coarse"),
)


def _limits() -> dict:
    return {
        "git_sha": "abc",
        "s2": {
            "gate": True,
            "peak_rss_bytes_max": 100,
            "rss_bytes_max": 100,
            "minor_faults_max": 50,
        },
        "s1": {"gate": True, "rss_bytes_per_define_max": 10.0},
        "s3": {"gate": True, "wall_ratio_max": 3.0},
        "s4": {"gate": True, "speedup_8_min": 1.5},
    }


def _sample() -> dict:
    return {
        "scenarios": {
            "s2": {"peak_rss_bytes": 80, "rss_bytes": 80, "minor_faults": 40, "exit": 0},
            "s1": {"rss_bytes_per_define": 4.0, "exit": 0, "points": []},
            "s3": {"wall_ratio": 2.0, "checks_ok": True, "exit": 0},
            "s4": {"speedup": {"8": 2.0}, "totals_ok": True, "exit": 0},
        }
    }


class AllocBenchGate(unittest.TestCase):
    def test_high_rss_names_the_metric(self) -> None:
        sample = _sample()
        sample["scenarios"]["s2"]["peak_rss_bytes"] = 10_000
        sample["scenarios"]["s2"]["minor_faults"] = 9_000
        text = "\n".join(alloc_bench.metric_failures(sample, _limits(), "git log abc..def"))
        self.assertIn("FAIL s2 peak_rss_bytes", text)
        self.assertIn("FAIL s2 minor_faults", text)
        self.assertIn("baseline", text)
        self.assertIn("current", text)
        self.assertIn("git log", text)

    def test_within_ceiling_passes(self) -> None:
        self.assertEqual(alloc_bench.metric_failures(_sample(), _limits(), "git log abc..def"), [])

    def test_empty_platform_is_record_only(self) -> None:
        sample = _sample()
        sample["scenarios"]["s2"]["peak_rss_bytes"] = 10**12
        self.assertEqual(alloc_bench.metric_failures(sample, {}, "git log abc..def"), [])

    def test_readme_and_nightly(self) -> None:
        readme = (ROOT / "tests" / "bench" / "README.md").read_text(encoding="utf-8")
        self.assertIn("alloc_bench.py", readme)
        nightly = (ROOT / ".github" / "workflows" / "nightly.yml").read_text(encoding="utf-8")
        self.assertIn("alloc_bench.py", nightly)
        self.assertIn("5637", nightly)

    def test_x86_baseline_empty(self) -> None:
        doc = json.loads((ROOT / "tests" / "bench" / "benchmark_baseline.json").read_text(encoding="utf-8"))
        platforms = doc["alloc"]["platforms"]
        self.assertEqual(platforms["linux-x86_64"], {})
        self.assertIn("linux-aarch64", platforms)

    def test_s2_ceiling_is_coarse(self) -> None:
        doc = json.loads((ROOT / "tests" / "bench" / "benchmark_baseline.json").read_text(encoding="utf-8"))
        plat = doc["alloc"]["platforms"]["linux-aarch64"]
        s2 = plat["s2"]
        self.assertTrue(s2["peak_rss_bytes_gate"])
        self.assertTrue(s2["minor_faults_gate"])
        self.assertGreater(s2["peak_rss_bytes_max"], 500 * 1024 * 1024)
        self.assertLess(s2["peak_rss_bytes_max"], 1024 * 1024 * 1024)
        self.assertGreater(s2["minor_faults_max"], s2["minor_faults_median"] * 2)
        self.assertLess(plat["ranges"]["s2.rss_bytes"], 0.2)
        self.assertGreater(plat["ranges"]["s2.rss_bytes"], 0.02)
        self.assertTrue(plat["s1"]["alloc_calls_per_define_gate"])
        self.assertTrue(plat["s3"]["wall_ratio_gate"])
        self.assertFalse(plat["s4"]["speedup_8_gate"])


if __name__ == "__main__":
    unittest.main()
