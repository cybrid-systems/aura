#!/usr/bin/env python3
"""clang-scan-deps installer prefers a matching binary already on the image.

The real LLVM tarball is not downloaded here. curl is a fixture that records
whether the fallback path ran.
"""

from __future__ import annotations

import os
import stat
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "ci" / "install_clang_scan_deps.sh"
COMMIT = "392dfff97d29b59d77e5cac6d4b6ef2585fe7117"
GOOD_COMPILER = f'#!/bin/sh\necho "clang version 23.1.5 (https://github.com/llvm/llvm-project {COMMIT})"\n'
BAD_COMPILER = "#!/bin/sh\necho 'clang version 22.1.0'\n"


def _exe(path: Path, text: str) -> None:
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def _scanner(text: str) -> str:
    return f"#!/bin/sh\n{text}\n"


class InstallClangScanDeps(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.dest = self.root / "dest" / "clang-scan-deps"
        self.dest.parent.mkdir()
        self.curl_log = self.root / "curl.log"
        self.cache = self.root / "cache"
        self.cache.mkdir()
        _exe(self.bin / "clang++", GOOD_COMPILER)
        _exe(
            self.bin / "curl",
            "#!/bin/sh\n"
            "out=\n"
            "prev=\n"
            'for a in "$@"; do\n'
            '  if [ "$prev" = "-o" ]; then out=$a; fi\n'
            "  prev=$a\n"
            "done\n"
            f'echo "$out" >> "{self.curl_log}"\n'
            f'cp "{self.root / "llvm.tar.xz"}" "$out"\n',
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _tar(self, body: str) -> None:
        payload = self.root / "payload" / "llvm" / "bin"
        payload.mkdir(parents=True)
        _exe(payload / "clang-scan-deps", _scanner(body))
        tar_path = self.root / "llvm.tar.xz"
        with tarfile.open(tar_path, "w:xz") as tar:
            tar.add(payload / "clang-scan-deps", arcname="llvm/bin/clang-scan-deps")

    def _run(self, *, compiler: str | None = None) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["PATH"] = f"{self.bin}{os.pathsep}/usr/bin{os.pathsep}/bin"
        env["CXX"] = str(self.bin / "clang++")
        env["SCAN_DEPS_DEST"] = str(self.dest)
        env["SCAN_DEPS_CACHE"] = str(self.cache)
        if compiler is not None:
            _exe(self.bin / "clang++", compiler)
        return subprocess.run(
            ["bash", str(SCRIPT)],
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_matching_install_does_not_download_or_overwrite(self) -> None:
        _exe(self.dest, _scanner('echo "LLVM version 23.1.5"\necho marker-image\n'))
        before = self.dest.read_bytes()
        _exe(self.cache / "clang-scan-deps", _scanner('echo "LLVM version 23.1.5"\necho marker-cache\n'))
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.dest.read_bytes(), before)
        self.assertFalse(self.curl_log.exists())
        self.assertIn("using installed clang-scan-deps", result.stdout)

    def test_missing_scanner_uses_the_tarball(self) -> None:
        self._tar('echo "LLVM version 23.1.5"\necho marker-download\n')
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertTrue(self.curl_log.exists())
        text = self.dest.read_text()
        self.assertIn("LLVM version 23.1.5", text)
        self.assertIn("marker-download", text)

    def test_version_mismatch_is_not_kept(self) -> None:
        _exe(self.dest, _scanner('echo "LLVM version 22.1.0"\necho marker-bad\n'))
        self._tar('echo "LLVM version 23.1.5"\necho marker-download\n')
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertTrue(self.curl_log.exists())
        text = self.dest.read_text()
        self.assertIn("LLVM version 23.1.5", text)
        self.assertNotIn("22.1.0", text)
        self.assertIn("marker-download", text)

    def test_compiler_mismatch_fails_before_download(self) -> None:
        _exe(self.dest, _scanner('echo "LLVM version 22.1.0"\necho marker-bad\n'))
        before = self.dest.read_bytes()
        result = self._run(compiler=BAD_COMPILER)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.curl_log.exists())
        self.assertEqual(self.dest.read_bytes(), before)
        self.assertIn("clang++ must be 23.1.5", result.stderr)

    def test_script_still_refuses_another_clang_major(self) -> None:
        script = SCRIPT.read_text()
        self.assertIn("llvmorg-23.1.5", script)
        self.assertIn("LLVM-23.1.5-Linux-X64.tar.xz", script)
        self.assertIn("LLVM-23.1.5-Linux-ARM64.tar.xz", script)
        self.assertNotIn("clang-tools-22", script)
        action = (ROOT / ".github" / "actions" / "clang-scan-deps" / "action.yml").read_text()
        self.assertIn("not replaced", action)


if __name__ == "__main__":
    unittest.main()
