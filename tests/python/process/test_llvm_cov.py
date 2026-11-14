#!/usr/bin/env python3
"""Issue #1933 — llvm-cov integration surface (static + tooling)."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class LlvmCov1933(unittest.TestCase):
    def test_cmake_module(self) -> None:
        p = ROOT / "cmake" / "AuraCoverage.cmake"
        self.assertTrue(p.is_file())
        text = p.read_text(encoding="utf-8")
        self.assertIn("AURA_ENABLE_COVERAGE", text)
        self.assertIn("fcoverage-mapping", text)
        self.assertIn("1933", text)

    def test_cmake_include(self) -> None:
        cm = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("AuraCoverage.cmake", cm)

    def test_preset(self) -> None:
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        names = {p["name"] for p in presets.get("configurePresets", [])}
        self.assertIn("coverage", names)
        cov = next(p for p in presets["configurePresets"] if p["name"] == "coverage")
        self.assertEqual(cov.get("binaryDir", ""), "${sourceDir}/build_coverage")
        cache = cov.get("cacheVariables", {})
        self.assertEqual(cache.get("AURA_ENABLE_COVERAGE"), "ON")

    def test_report_script_importable(self) -> None:
        script = ROOT / "scripts" / "tools" / "llvm_cov_report.py"
        self.assertTrue(script.is_file())
        mod = load_module("llvm_cov_report", script)
        self.assertTrue(callable(mod.main))
        self.assertTrue(callable(mod.parse_totals_from_export))
        self.assertTrue(callable(mod.find_tool))

    def test_parse_totals_helper(self) -> None:
        script = ROOT / "scripts" / "tools" / "llvm_cov_report.py"
        mod = load_module("llvm_cov_report2", script)
        sample = {
            "data": [
                {
                    "totals": {
                        "lines": {"count": 100, "covered": 75, "percent": 75.0},
                        "regions": {"percent": 70.0},
                    },
                    "files": [
                        {
                            "filename": "src/compiler/evaluator.ixx",
                            "summary": {"lines": {"count": 40, "covered": 30}},
                        }
                    ],
                }
            ]
        }
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "c.json"
            p.write_text(json.dumps(sample), encoding="utf-8")
            totals = mod.parse_totals_from_export(p)
            self.assertAlmostEqual(totals["line_pct"], 75.0)
            pct = mod.module_line_pct(p, "evaluator")
            self.assertIsNotNone(pct)
            self.assertAlmostEqual(pct, 75.0)

    def test_build_py_check_tools(self) -> None:
        r = subprocess.run(
            [sys.executable, str(ROOT / "build.py"), "coverage", "--check-tools"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("coverage", r.stdout.lower() + r.stderr.lower())

    def test_docs(self) -> None:
        testing = ROOT / "tests" / "COVERAGE.md"
        self.assertTrue(testing.is_file())
        text = testing.read_text(encoding="utf-8")
        self.assertIn("Coverage policy", text)
        self.assertIn("coverage", text.lower())

    def test_nightly_workflow(self) -> None:
        yml = (ROOT / ".github" / "workflows" / "nightly.yml").read_text(encoding="utf-8")
        self.assertIn("coverage", yml)
        self.assertIn("build.py coverage", yml)


PROCESS_THEME = "llvm-cov"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ()
CASES = (
    (
        1933,
        "test_cmake_module",
        "LlvmCov1933.test_cmake_module",
    ),
    (
        1933,
        "test_cmake_include",
        "LlvmCov1933.test_cmake_include",
    ),
    (
        1933,
        "test_preset",
        "LlvmCov1933.test_preset",
    ),
    (
        1933,
        "test_report_script_importable",
        "LlvmCov1933.test_report_script_importable",
    ),
    (
        1933,
        "test_parse_totals_helper",
        "LlvmCov1933.test_parse_totals_helper",
    ),
    (
        1933,
        "test_build_py_check_tools",
        "LlvmCov1933.test_build_py_check_tools",
    ),
    (
        1933,
        "test_docs",
        "LlvmCov1933.test_docs",
    ),
    (
        1933,
        "test_nightly_workflow",
        "LlvmCov1933.test_nightly_workflow",
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
