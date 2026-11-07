#!/usr/bin/env python3
"""CI toolchain image pin.

Active workflows that compile Aura use ghcr.io/cybrid-systems/aura-ci:v0.1.0.
The runtime image published for users stays ghcr.io/cybrid-systems/aura.
dev:v1.1.1 is not an active CI image. nightly jobs that still name
dev:v1.0.5 are outside this pin.
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
OLD = "ghcr.io/cybrid-systems/dev:v1.1.1"
NEW = "ghcr.io/cybrid-systems/aura-ci:v0.1.0"
RUNTIME = "ghcr.io/cybrid-systems/aura:"

# Jobs that compile Aura inside the toolchain container.
PINNED = {
    ".github/workflows/ci.yml": 8,
    ".github/workflows/dist.yml": 1,
    ".github/workflows/nightly.yml": 2,
    ".github/workflows/release.yml": 1,
}
# Compile jobs also install libkrb5-dev. deployment-health only runs a
# downloaded binary, so ci.yml has one fewer action use than image pins.
KRB5_ACTION = "uses: ./.github/actions/ci-krb5"
KRB5_USES = {
    ".github/workflows/ci.yml": 7,
    ".github/workflows/dist.yml": 1,
    ".github/workflows/nightly.yml": 2,
    ".github/workflows/release.yml": 1,
}


class CiToolchainImage(unittest.TestCase):
    def test_v111_is_gone_from_workflows(self) -> None:
        for path in sorted(WORKFLOWS.glob("*.yml")):
            text = path.read_text()
            self.assertNotIn(OLD, text, path.name)

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
        self.assertIn("llvmorg-${ver}", script)
        self.assertIn("LLVM-${ver}-Linux-X64.tar.xz", script)
        self.assertIn("LLVM-${ver}-Linux-ARM64.tar.xz", script)
        self.assertNotIn("expected_ver=", script)
        self.assertNotIn("expected_commit=", script)
        self.assertNotIn("392dfff97d29", script)
        self.assertNotIn("clang-tools-22", script)
        action = (ROOT / ".github" / "actions" / "clang-scan-deps" / "action.yml").read_text()
        self.assertNotIn("23.1.5", action)
        self.assertIn("steps.clangver.outputs.version", action)
        self.assertIn("runner.arch", action)
        self.assertNotIn("dev:v1.1.1", script)
        gate = (WORKFLOWS / "ci.yml").read_text()
        self.assertIn("uses: ./.github/actions/clang-scan-deps", gate)
        self.assertIn("CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=/usr/local/bin/clang-scan-deps", gate)
        self.assertIn("runs-on: ubuntu-24.04-arm", gate)
        self.assertIn("clang-module-arm64", gate)

    def test_runtime_image_is_not_the_toolchain(self) -> None:
        for path in (ROOT / "templates").rglob("ci.yml"):
            text = path.read_text()
            self.assertIn(RUNTIME, text, path)
            self.assertNotIn("aura-ci", text, path)
        dist = (WORKFLOWS / "dist.yml").read_text()
        self.assertIn("ghcr.io/cybrid-systems/aura:<sha>", dist)
        self.assertNotIn("aura-ci:<sha>", dist)


if __name__ == "__main__":
    unittest.main()
