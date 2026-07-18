#!/usr/bin/env python3
"""Unit tests for scripts/audit_catch_silent_swallow.py (#1669 / #615)."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "audit_catch_silent_swallow.py"


def _load():
    spec = importlib.util.spec_from_file_location("audit_catch_silent_swallow", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestCatchSilentSwallowAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def _scan(self, src: str) -> list[tuple[int, str, str]]:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "snippet.cpp"
            p.write_text(src, encoding="utf-8")
            return self.m.scan_file(p)

    def test_marked_ok(self):
        hits = self._scan(
            """
            try { risky(); }
            catch (...) {
                // [SILENCE-PRIM-#615] intentional #f
                return make_bool(false);
            }
            """
        )
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0][1], "marked")

    def test_unmarked_flagged(self):
        hits = self._scan(
            """
            try { risky(); }
            catch (...) {
                return make_bool(false);
            }
            """
        )
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0][1], "unmarked")

    def test_comment_only_not_catch(self):
        hits = self._scan(
            """
            // with post-try catch (...) blocks) instead of silent
            void f() {}
            """
        )
        self.assertEqual(hits, [])

    def test_src_compiler_clean_strict(self):
        hits = self.m.collect_hits(ROOT / "src" / "compiler", unmarked_only=True)
        self.assertEqual(hits, [], f"unmarked catch(...): {hits}")


if __name__ == "__main__":
    unittest.main()
