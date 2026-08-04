#!/usr/bin/env python3
"""Unit tests for scripts/coverage/checks/check_aot_env_linear_stamp_coverage.py (#2091 / #2168).

AC coverage:
  - literal (0,0) is flagged without annotation
  - `# 2091-allow-zero` / `# 2091-legacy` on same or previous line opts out
  - 3-arg (defuse-only) calls are flagged in production-style snippets
  - live / non-zero args are clean
  - comment mentions of mangle_aot_name are not call sites
  - --self-test exits 0 against the real bridge TU
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # tests/python → repo
SCRIPT = ROOT / "scripts" / "coverage" / "checks" / "check_aot_env_linear_stamp_coverage.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_aot_env_linear_stamp", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestAotEnvLinearStampGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def _scan(self, src: str) -> list[tuple[int, str, str, str]]:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "snippet.cpp"
            p.write_text(src, encoding="utf-8")
            return self.m.scan_file(p)

    def test_literal_zero_zero_flagged(self):
        hits = self._scan(
            """
            std::string f() {
                return mangle_aot_name("foo", 1, 0, 0, 0);
            }
            """
        )
        self.assertTrue(hits, hits)
        self.assertEqual(hits[0][2], "0")
        self.assertEqual(hits[0][3], "0")

    def test_allow_zero_same_line(self):
        hits = self._scan(
            """
            std::string f() {
                return mangle_aot_name("foo", 1, 0, 0, 0); // # 2091-allow-zero deliberate
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_allow_zero_previous_line(self):
        hits = self._scan(
            """
            std::string f() {
                // # 2091-allow-zero — probe path
                return aot_link_name("bar", 2, 0, 0, 0);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_legacy_annotation(self):
        hits = self._scan(
            """
            std::string f() {
                // # 2091-legacy
                return mangle_aot_name("baz", 0, 0, 0, 0);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_live_args_clean(self):
        hits = self._scan(
            """
            std::string f(std::uint64_t env, std::uint8_t lin) {
                return mangle_aot_name("foo", 1, 3, env, lin);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_one_nonzero_clean(self):
        hits = self._scan(
            """
            std::string f() {
                return mangle_aot_name("foo", 1, 0, 7, 0);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_three_arg_flagged(self):
        hits = self._scan(
            """
            std::string f() {
                return mangle_aot_name("foo", 1, 0);
            }
            """
        )
        self.assertTrue(hits, hits)
        self.assertEqual(hits[0][2], "<3-arg>")

    def test_three_arg_allowed(self):
        hits = self._scan(
            """
            std::string f() {
                // # 2091-allow-zero
                return mangle_aot_name("foo", 1, 0);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_comment_not_call_site(self):
        hits = self._scan(
            """
            // do not call mangle_aot_name("x", 0, 0, 0, 0) here
            /* aot_link_name("y", 0, 0, 0, 0) in comment */
            std::string f(std::uint64_t e, std::uint8_t l) {
                return mangle_aot_name("ok", 1, 1, e, l);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_self_test_cli(self):
        r = subprocess.run(
            [sys.executable, str(SCRIPT), "--self-test"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("SELF-TEST OK", r.stdout)

    def test_full_scan_clean(self):
        r = subprocess.run(
            [sys.executable, str(SCRIPT)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK:", r.stdout)


if __name__ == "__main__":
    unittest.main()
