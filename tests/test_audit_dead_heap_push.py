#!/usr/bin/env python3
"""Unit tests for scripts/audit_dead_heap_push.py (#1488 / #1668)."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "audit_dead_heap_push.py"


def _load():
    spec = importlib.util.spec_from_file_location("audit_dead_heap_push", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestDeadHeapPushAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def _scan_src(self, src: str) -> list[tuple[int, str, str, str]]:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "snippet.cpp"
            p.write_text(src, encoding="utf-8")
            return self.m.scan_file(p, rel="snippet.cpp")

    def test_used_index_clean(self):
        hits = self._scan_src(
            """
            void f(Evaluator& ev) {
                auto idx = ev.string_heap_.size();
                ev.string_heap_.push_back("hello");
                return make_string(idx);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_void_only_is_dead(self):
        hits = self._scan_src(
            """
            void f(Evaluator& ev) {
                auto idx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::to_string(n));
                (void)idx;  // warning silence only
                return make_int(n);
            }
            """
        )
        kinds = [h[2] for h in hits]
        self.assertIn("unused-index", kinds, hits)

    def test_bare_push_dead(self):
        hits = self._scan_src(
            """
            void load(Evaluator& ev, const std::string& resolved) {
                modules_.push_back(mod_env);
                string_heap_.push_back(resolved);  // never captures index
                module_names_.push_back(resolved);
            }
            """
        )
        kinds = [h[2] for h in hits]
        self.assertIn("bare-push", kinds, hits)

    def test_comment_not_flagged(self):
        hits = self._scan_src(
            """
            void f() {
                // do not string_heap_.push_back(resolved) — removed in #1668
                module_names_.push_back(resolved);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_multiline_size_then_push_used(self):
        hits = self._scan_src(
            """
            void f(Evaluator& ev) {
                auto sidx = ev.string_heap_.size();
                std::string script = "line1\\n"
                                     "line2\\n"
                                     "line3\\n"
                                     "line4\\n"
                                     "line5\\n";
                ev.string_heap_.push_back(script);
                return make_string(sidx);
            }
            """
        )
        self.assertEqual(hits, [], hits)

    def test_src_tree_clean_strict(self):
        """Production src/ must stay clean under the #1668 gate."""
        hits = self.m.collect_hits(ROOT / "src")
        self.assertEqual(hits, [], f"unexpected dead heap push candidates: {hits}")


if __name__ == "__main__":
    unittest.main()
