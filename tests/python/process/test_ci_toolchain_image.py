#!/usr/bin/env python3
"""CI toolchain image pin.

Active workflows that compile Aura use ghcr.io/cybrid-systems/aura-ci:v0.1.1
(CMake 4.5.0). The runtime image published for users stays
ghcr.io/cybrid-systems/aura. dev:v1.1.1 and dev:v1.0.5 are not active CI
images.
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"
OLD = "ghcr.io/cybrid-systems/dev:v1.1.1"
OLD_DEV = "ghcr.io/cybrid-systems/dev:v1.0.5"
NEW = "ghcr.io/cybrid-systems/aura-ci:v0.1.1"
RUNTIME = "ghcr.io/cybrid-systems/aura:"

# Jobs that compile Aura inside the toolchain container.
PINNED = {
    ".github/workflows/ci.yml": 9,
    ".github/workflows/dist.yml": 1,
    ".github/workflows/nightly.yml": 7,
    ".github/workflows/release.yml": 1,
}
# Compile jobs also install libkrb5-dev. deployment-health only runs a
# downloaded binary, so ci.yml has one fewer action use than image pins.
KRB5_ACTION = "uses: ./.github/actions/ci-krb5"
KRB5_USES = {
    ".github/workflows/ci.yml": 8,
    ".github/workflows/dist.yml": 1,
    ".github/workflows/nightly.yml": 7,
    ".github/workflows/release.yml": 1,
}


class CiToolchainImage(unittest.TestCase):
    def test_v111_is_gone_from_workflows(self) -> None:
        for path in sorted(WORKFLOWS.glob("*.yml")):
            text = path.read_text()
            self.assertNotIn(OLD, text, path.name)
            self.assertNotIn(OLD_DEV, text, path.name)

    def test_pinned_jobs_use_aura_ci(self) -> None:
        for rel, count in PINNED.items():
            text = (ROOT / rel).read_text()
            self.assertEqual(text.count(NEW), count, rel)
            self.assertNotIn(OLD, text, rel)

    def test_compile_jobs_install_krb5_link_names(self) -> None:
        action = ROOT / ".github" / "actions" / "ci-krb5" / "action.yml"
        body = action.read_text()
        self.assertIn("libkrb5-dev", body)
        self.assertNotIn(OLD, body)
        for rel, count in KRB5_USES.items():
            text = (ROOT / rel).read_text()
            self.assertEqual(text.count(KRB5_ACTION), count, rel)

    def test_scan_deps_is_the_matching_llvm_release(self) -> None:
        script = (ROOT / "scripts" / "ci" / "install_clang_scan_deps.sh").read_text()
        for banned in ("curl", "wget", "apt-get", "llvmorg-", "expected_ver=", "expected_commit=", "392dfff97d29"):
            self.assertNotIn(banned, script)
        action = (ROOT / ".github" / "actions" / "clang-scan-deps" / "action.yml").read_text()
        self.assertNotIn("23.1.5", action)
        self.assertNotIn("actions/cache", action)
        self.assertIn("outputs:", action)
        self.assertIn("steps.locate.outputs.path", action)
        gate = (WORKFLOWS / "ci.yml").read_text()
        self.assertIn("uses: ./.github/actions/clang-scan-deps", gate)
        self.assertIn("id: clang-scan-deps", gate)
        self.assertIn(
            "CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=${{ steps.clang-scan-deps.outputs.path }}",
            gate,
        )
        self.assertNotIn("/usr/local/bin/clang-scan-deps", gate)
        self.assertIn("runs-on: ubuntu-24.04-arm", gate)
        self.assertIn("clang-module-arm64", gate)
        self.assertIn("Build Clang module BMIs", gate)

    def test_runtime_image_is_not_the_toolchain(self) -> None:
        for path in (ROOT / "templates").rglob("ci.yml"):
            text = path.read_text()
            self.assertIn(RUNTIME, text, path)
            self.assertNotIn("aura-ci", text, path)
        dist = (WORKFLOWS / "dist.yml").read_text()
        self.assertIn("ghcr.io/cybrid-systems/aura:<sha>", dist)
        self.assertNotIn("aura-ci:<sha>", dist)

    def test_cmake_baseline_is_45(self) -> None:
        text = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn("cmake_minimum_required(VERSION 4.5)", text)
        self.assertIn('SPDX_LICENSE "Apache-2.0"', text)
        for banned in ("CXX_MODULE_STD", "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD"):
            self.assertNotIn(banned, text)

    def test_import_std_gate_is_gone_everywhere(self) -> None:
        for rel in (
            "cmake/AuraTest.cmake",
            "cmake/AuraIssueBundles.cmake",
            "scripts/tools/gen_issue_bundles.py",
            "scripts/tools/fold_issue_test_wave.py",
            "build.py",
        ):
            text = (ROOT / rel).read_text()
            self.assertNotIn("CXX_MODULE_STD", text, rel)
            self.assertNotIn("CMAKE_EXPERIMENTAL_CXX_IMPORT_STD", text, rel)

    def test_presets_schema13_and_flags_match_build_py(self) -> None:
        import json

        data = json.loads((ROOT / "CMakePresets.json").read_text())
        self.assertEqual(data["version"], 13)
        presets = {p["name"]: p for p in data["configurePresets"]}
        for name in ("default", "release", "asan", "ubsan", "repro", "tsan", "coverage"):
            self.assertIn(name, presets)
        base = presets["aura-base"]
        self.assertTrue(base.get("hidden"))
        self.assertEqual(base["warnings"], {"strict": True, "nonTargetDirective": True})
        self.assertEqual(base["cacheVariables"]["AURA_CONFIGURE_PRESET"], "${configurePresetName}")
        repro = presets["repro"]["cacheVariables"]
        self.assertIn("-fmacro-prefix-map=${sourceDir}=.", repro["CMAKE_CXX_FLAGS"])
        self.assertIn(
            "-fdebug-prefix-map=${sourceDir}/build_repro=build",
            repro["CMAKE_CXX_FLAGS"],
        )
        self.assertIn("-fno-ident", repro["CMAKE_CXX_FLAGS"])
        tsan = presets["tsan"]["cacheVariables"]["CMAKE_CXX_FLAGS"]
        self.assertIn("-Wno-error=mismatched-new-delete", tsan)
        self.assertIn("-Wno-error=tsan", tsan)
        build_py = (ROOT / "build.py").read_text()
        for flag in ("-Wno-error=mismatched-new-delete", "-Wno-error=tsan"):
            self.assertIn(flag, build_py)
        self.assertIn("-Werror=experimental", build_py)
        self.assertIn("-Wstrict", build_py)

    def test_capabilities_assertion_runs_in_ci(self) -> None:
        text = (WORKFLOWS / "ci.yml").read_text()
        self.assertIn("Toolchain image assertion (#5201)", text)
        self.assertIn("'capabilities'", text)


PROCESS_THEME = "ci-toolchain"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ()
CASES = (
    (
        0,
        "test_v111_is_gone_from_workflows",
        "CiToolchainImage.test_v111_is_gone_from_workflows",
    ),
    (
        0,
        "test_pinned_jobs_use_aura_ci",
        "CiToolchainImage.test_pinned_jobs_use_aura_ci",
    ),
    (
        0,
        "test_compile_jobs_install_krb5_link_names",
        "CiToolchainImage.test_compile_jobs_install_krb5_link_names",
    ),
    (
        0,
        "test_scan_deps_is_the_matching_llvm_release",
        "CiToolchainImage.test_scan_deps_is_the_matching_llvm_release",
    ),
    (
        0,
        "test_runtime_image_is_not_the_toolchain",
        "CiToolchainImage.test_runtime_image_is_not_the_toolchain",
    ),
    (
        0,
        "test_cmake_baseline_is_45",
        "CiToolchainImage.test_cmake_baseline_is_45",
    ),
    (
        0,
        "test_import_std_gate_is_gone_everywhere",
        "CiToolchainImage.test_import_std_gate_is_gone_everywhere",
    ),
    (
        0,
        "test_presets_schema13_and_flags_match_build_py",
        "CiToolchainImage.test_presets_schema13_and_flags_match_build_py",
    ),
    (
        0,
        "test_capabilities_assertion_runs_in_ci",
        "CiToolchainImage.test_capabilities_assertion_runs_in_ci",
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
