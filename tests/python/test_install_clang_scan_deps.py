#!/usr/bin/env python3
"""clang-scan-deps follows the clang++ that is actually selected.

Versions in these tests are fixtures. The LLVM tarball is not downloaded.
"""

from __future__ import annotations

import os
import platform
import stat
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "ci" / "install_clang_scan_deps.sh"
ARCH = {"x86_64": "X64", "aarch64": "ARM64", "arm64": "ARM64"}[platform.machine()]
MACHINE = platform.machine()


def _exe(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def _compiler(version: str) -> str:
    return f'#!/bin/sh\necho "clang version {version} (https://github.com/llvm/llvm-project abc)"\n'


def _scanner(version: str, marker: str) -> str:
    return f'#!/bin/sh\necho "LLVM version {version}"\necho {marker}\n'


class InstallClangScanDeps(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.dest = self.root / "dest" / "clang-scan-deps"
        self.curl_log = self.root / "curl.log"
        self.cache = self.root / "cache"
        self.cache.mkdir()
        _exe(
            self.bin / "curl",
            "#!/bin/sh\n"
            "out=\n"
            "prev=\n"
            'for a in "$@"; do\n'
            '  if [ "$prev" = "-o" ]; then out=$a; fi\n'
            "  prev=$a\n"
            "done\n"
            f'printf "%s\\n" "$@" >> "{self.curl_log}"\n'
            f'cp "{self.root / "llvm.tar.xz"}" "$out"\n',
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _tar(self, version: str, marker: str) -> None:
        payload = self.root / "payload" / "llvm" / "bin"
        _exe(payload / "clang-scan-deps", _scanner(version, marker))
        with tarfile.open(self.root / "llvm.tar.xz", "w:xz") as tar:
            tar.add(payload / "clang-scan-deps", arcname="llvm/bin/clang-scan-deps")

    def _run(self, version: str | None, *, raw: str | None = None) -> subprocess.CompletedProcess[str]:
        if raw is None:
            assert version is not None
            text = _compiler(version)
        else:
            text = raw
        _exe(self.bin / "clang++", text)
        env = os.environ.copy()
        env["PATH"] = f"{self.bin}{os.pathsep}/usr/bin{os.pathsep}/bin"
        env["CXX"] = str(self.bin / "clang++")
        env["SCAN_DEPS_DEST"] = str(self.dest)
        env["SCAN_DEPS_CACHE"] = str(self.cache)
        return subprocess.run(
            ["bash", str(SCRIPT)],
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_two_installed_versions_do_not_download(self) -> None:
        for version in ("23.1.5", "22.1.2"):
            with self.subTest(version=version):
                _exe(self.dest, _scanner(version, "marker-image"))
                before = self.dest.read_bytes()
                _exe(
                    self.cache / version / MACHINE / "clang-scan-deps",
                    _scanner(version, "marker-cache"),
                )
                result = self._run(version)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.dest.read_bytes(), before)
                self.assertFalse(self.curl_log.exists())
                self.assertIn(f"using installed clang-scan-deps {version}", result.stdout)
                self.assertIn(f"matched clang++ LLVM version {version}", result.stderr)

    def test_version_mismatch_downloads_the_compiler_release(self) -> None:
        _exe(self.dest, _scanner("22.1.2", "marker-bad"))
        self._tar("23.1.5", "marker-download")
        result = self._run("23.1.5")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        logged = self.curl_log.read_text()
        self.assertIn(
            f"https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/LLVM-23.1.5-Linux-{ARCH}.tar.xz",
            logged,
        )
        self.assertNotIn("22.1.2", logged)
        text = self.dest.read_text()
        self.assertIn("LLVM version 23.1.5", text)
        self.assertIn("marker-download", text)
        self.assertNotIn("22.1.2", text)

    def test_cache_is_isolated_by_version_and_arch(self) -> None:
        self.dest.unlink(missing_ok=True)
        _exe(self.cache / "clang-scan-deps", _scanner("22.1.2", "marker-flat"))
        _exe(
            self.cache / "22.1.2" / MACHINE / "clang-scan-deps",
            _scanner("22.1.2", "marker-other"),
        )
        _exe(
            self.cache / "23.1.5" / "other-arch" / "clang-scan-deps",
            _scanner("23.1.5", "marker-wrong-arch"),
        )
        _exe(
            self.cache / "23.1.5" / MACHINE / "clang-scan-deps",
            _scanner("23.1.5", "marker-cache"),
        )
        result = self._run("23.1.5")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertFalse(self.curl_log.exists())
        text = self.dest.read_text()
        self.assertIn("marker-cache", text)
        self.assertNotIn("marker-flat", text)
        self.assertNotIn("marker-other", text)
        self.assertNotIn("marker-wrong-arch", text)
        self.assertIn(f"using cached clang-scan-deps 23.1.5 {MACHINE}", result.stdout)

    def test_git_snapshot_fails_closed(self) -> None:
        result = self._run(
            None,
            raw='#!/bin/sh\necho "clang version 23.0.0git (https://github.com/llvm/llvm-project abc)"\n',
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.curl_log.exists())
        self.assertFalse(self.dest.exists())
        self.assertIn("cannot resolve an LLVM release", result.stderr)

    def test_release_candidate_fails_closed(self) -> None:
        result = self._run(None, raw='#!/bin/sh\necho "clang version 23.1.5-rc1"\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.curl_log.exists())
        self.assertIn("cannot resolve an LLVM release", result.stderr)

    def test_names_are_not_pinned_to_one_release(self) -> None:
        script = SCRIPT.read_text()
        action = (ROOT / ".github" / "actions" / "clang-scan-deps" / "action.yml").read_text()
        self.assertNotIn("23.1.5", script)
        self.assertNotIn("23.1.5", action)
        self.assertNotIn("392dfff97d29", script)
        self.assertNotIn("392dfff97d29", action)
        self.assertIn("name: Install clang-scan-deps\n", action)
        self.assertIn("steps.clangver.outputs.version", action)
        self.assertIn("runner.arch", action)


if __name__ == "__main__":
    unittest.main()
