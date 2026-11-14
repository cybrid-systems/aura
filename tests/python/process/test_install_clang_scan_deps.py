#!/usr/bin/env python3
"""clang-scan-deps is taken from the image. The script never downloads."""

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "ci" / "install_clang_scan_deps.sh"


def _exe(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def _compiler(version: str) -> str:
    return f'#!/bin/sh\necho "clang version {version} (https://github.com/llvm/llvm-project abc)"\n'


def _scanner(version: str) -> str:
    return f'#!/bin/sh\necho "LLVM version {version}"\n'


class LocateClangScanDeps(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bindir = self.root / "bin"
        self.compdir = self.root / "compiler"
        self.netlog = self.root / "net.log"
        self.bindir.mkdir()
        self.compdir.mkdir()
        for name in ("curl", "wget", "apt", "apt-get"):
            _exe(self.bindir / name, f'#!/bin/sh\necho {name} >> "{self.netlog}"\nexit 99\n')

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _run(self, version: str = "23.1.5") -> subprocess.CompletedProcess[str]:
        _exe(self.compdir / "clang++", _compiler(version))
        env = os.environ.copy()
        env["PATH"] = f"{self.bindir}{os.pathsep}/usr/bin{os.pathsep}/bin"
        env["CXX"] = str(self.compdir / "clang++")
        env.pop("GITHUB_OUTPUT", None)
        env.pop("GITHUB_ENV", None)
        return subprocess.run(
            ["bash", str(SCRIPT)],
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_matching_tool_beside_compiler_does_not_download(self) -> None:
        scanner = self.compdir / "clang-scan-deps"
        _exe(scanner, _scanner("23.1.5"))
        result = self._run("23.1.5")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(scanner.resolve()))
        self.assertFalse(self.netlog.exists())
        self.assertIn("using clang-scan-deps 23.1.5", result.stderr)

    def test_matching_tool_on_path_does_not_download(self) -> None:
        scanner = self.bindir / "clang-scan-deps"
        _exe(scanner, _scanner("22.1.2"))
        result = self._run("22.1.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(scanner.resolve()))
        self.assertFalse(self.netlog.exists())

    def test_missing_tool_fails_with_no_download(self) -> None:
        result = self._run("23.1.5")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.netlog.exists())
        self.assertIn("aura-ci image has no clang-scan-deps matching clang++ 23.1.5", result.stderr)
        self.assertIn(str(self.compdir / "clang-scan-deps"), result.stderr)
        self.assertIn("/usr/lib/llvm-23/bin/clang-scan-deps", result.stderr)
        self.assertEqual(result.stdout.strip(), "")

    def test_version_mismatch_fails_with_no_download(self) -> None:
        _exe(self.compdir / "clang-scan-deps", _scanner("22.1.2"))
        result = self._run("23.1.5")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.netlog.exists())
        self.assertIn("version does not match 23.1.5", result.stderr)
        self.assertIn(str((self.compdir / "clang-scan-deps").resolve()), result.stderr)
        self.assertEqual(result.stdout.strip(), "")

    def test_script_has_no_download_or_package_install(self) -> None:
        text = SCRIPT.read_text()
        for banned in ("curl", "wget", "apt-get", "llvmorg-", "download "):
            self.assertNotIn(banned, text)
        action = (ROOT / ".github" / "actions" / "clang-scan-deps" / "action.yml").read_text()
        self.assertNotIn("actions/cache", action)
        self.assertNotIn("23.1.5", action)
        self.assertIn("steps.locate.outputs.path", action)


PROCESS_THEME = "install-clang"
PROCESS_TIER = "fast"
PROCESS_TIMEOUT = 180
PARTS = ()
CASES = (
    (
        0,
        "test_matching_tool_beside_compiler_does_not_download",
        "LocateClangScanDeps.test_matching_tool_beside_compiler_does_not_download",
    ),
    (
        0,
        "test_matching_tool_on_path_does_not_download",
        "LocateClangScanDeps.test_matching_tool_on_path_does_not_download",
    ),
    (
        0,
        "test_missing_tool_fails_with_no_download",
        "LocateClangScanDeps.test_missing_tool_fails_with_no_download",
    ),
    (
        0,
        "test_version_mismatch_fails_with_no_download",
        "LocateClangScanDeps.test_version_mismatch_fails_with_no_download",
    ),
    (
        0,
        "test_script_has_no_download_or_package_install",
        "LocateClangScanDeps.test_script_has_no_download_or_package_install",
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
