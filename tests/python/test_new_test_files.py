#!/usr/bin/env python3
"""Classifier for newly added issue-numbered test files. No issue-farm compile."""

from __future__ import annotations

import importlib.util
import io
import subprocess
import unittest
from contextlib import redirect_stderr
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "tools" / "check_new_test_files.py"
# CI containers run git as a different uid than the checkout owner.
GIT = ["git", "-c", "safe.directory=*"]


def _load():
    spec = importlib.util.spec_from_file_location("check_new_test_files", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestBannedNames(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def test_added_names_are_banned(self) -> None:
        banned = (
            "tests/python/test_issue_5226_demo.py",
            "tests/python/test_widget_5226.py",
            "tests/python/test_issue_4838_4846.py",
            "tests/suite/widget_5226.aura",
            "tests/suite/issue_5226_widget.aura",
            "tests/suite/issue_5226.aura",
            "tests/suite/string_join_5185.aura",
            "tests/suite/foo_5133.aura",
            "tests/suite/zzz_4242.aura",
            "scripts/coverage/checks/check_widget_5226.py",
            "scripts/coverage/checks/check_5226_widget.py",
            "scripts/coverage/checks/check_5226.py",
            "scripts/coverage/checks/check_widget_5226_extra.py",
            "tests/compiler/test_issue_5226.cpp",
            "tests/issues/test_issue_5226.cpp",
            "tests/test_new.cpp",
            "tests/test_foo_5226.cpp",
        )
        for path in banned:
            self.assertTrue(self.m.is_banned(path), path)

    def test_theme_names_are_legal(self) -> None:
        legal = (
            "tests/python/test_aura_file_runner.py",
            "tests/python/test_run_home_cover.py",
            "tests/python/test_new_test_files.py",
            "tests/suite/stdlib.aura",
            "tests/suite/parser_unclosed_list.aura",
            "tests/suite/string_join.aura",
            "scripts/coverage/checks/check_test_binding.py",
            "tests/compiler/test_linear_cross_closure.cpp",
            "tests/core/test_arena_batch.cpp",
            "tests/python/test_widget_52.py",
            "tests/python/test_widget_v2.py",
            "tests/suite/foo_12.aura",
            "tests/suite/foo_123456.aura",
            "tests/regression/foo_1234.aura",
            "scripts/check_widget_5226.py",
            "scripts/coverage/checks/check_foo_123456.py",
            "lib/std/test.aura",
        )
        for path in legal:
            self.assertFalse(self.m.is_banned(path), path)

    def test_historical_names_match_but_are_not_an_add_by_themselves(self) -> None:
        # The name is banned. Only an add fails. These files already exist.
        self.assertTrue(self.m.is_banned("tests/python/test_issue_5198_export_contracts.py"))
        self.assertTrue(self.m.is_banned("tests/suite/list_srfi_5133.aura"))
        self.assertTrue(self.m.is_banned("tests/suite/issue_5066_fiber_fastpath.aura"))

    def test_each_positive_matches_a_published_pattern(self) -> None:
        samples = {
            "tests/python/test_issue_5226_demo.py": r"^tests/(?:.+/)?test_issue_.*\.py$",
            "tests/suite/issue_5226.aura": r"^tests/suite/issue_[0-9]{3,5}\.aura$",
            "scripts/coverage/checks/check_5226.py": r"^scripts/coverage/checks/check_[0-9]{3,5}\.py$",
            "tests/test_new.cpp": r"^tests/test_[a-z].*\.cpp$",
        }
        for path, pattern in samples.items():
            self.assertIn(pattern, self.m.BANNED_PATTERNS)
            self.assertTrue(self.m.is_banned(path), path)


class TestQueryAndRefusal(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def test_surface_query_prints_a_home(self) -> None:
        cases = {
            "tests/suite/string_join_5185.aura": "tests/suite/string_join.aura",
            "tests/suite/iota_5192.aura": "tests/suite/list_srfi_5133.aura",
            "tests/suite/fiber_cancel_5064.aura": "tests/suite/issue_5064_fiber_cancel_text.aura",
            "tests/python/test_help_5226.py": "tests/python/test_issue_4935_help.py",
        }
        for path, home in cases.items():
            text = self.m.format_refusal([path])
            self.assertIn(f"home: {home}", text, path)
            self.assertIn("append:", text, path)
            self.assertIn("Genealogy: tests/HOMES.md", text)
            self.assertEqual(text.count("when the surface has no home yet:"), 1)

    def test_empty_stem_does_not_guess_a_path(self) -> None:
        text = self.m.format_refusal(["tests/python/test_issue_5226.py"])
        self.assertIn('not found: no home for "<feature>"', text)
        self.assertIn('python3 tests/run.py home "<feature>"', text)
        self.assertIn("Do not guess a file.", text)
        self.assertNotIn("home:", text)
        self.assertEqual(text.count("when the surface has no home yet:"), 1)

    def test_unknown_surface_is_a_miss(self) -> None:
        text = self.m.format_refusal(["tests/python/test_issue_5226_demo.py"])
        self.assertIn('not found: no home for "demo"', text)
        self.assertNotIn("home:", text)

    def test_create_rule_is_printed_once_for_many_files(self) -> None:
        text = self.m.format_refusal(
            [
                "tests/suite/string_join_5185.aura",
                "tests/python/test_issue_5226_demo.py",
                "scripts/coverage/checks/check_5226.py",
            ]
        )
        self.assertEqual(text.count("when the surface has no home yet:"), 1)
        self.assertIn("home: tests/suite/string_join.aura", text)
        self.assertIn('not found: no home for "demo"', text)
        self.assertIn('not found: no home for "<feature>"', text)
        self.assertIn("Do not add a check script", text)


class TestCollectAdded(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = _load()

    def test_untracked_banned_file_is_added_and_cached_ignores_it(self) -> None:
        rel = "tests/python/test_issue_5226_demo.py"
        path = ROOT / rel
        self.assertFalse(path.exists())
        try:
            path.write_text("# probe\n", encoding="utf-8")
            self.assertIn(rel, self.m.collect_added("gate"))
            self.assertNotIn(rel, self.m.collect_added("cached"))
            self.assertIn(rel, self.m.banned_added("gate"))
            err = io.StringIO()
            with redirect_stderr(err):
                code = self.m.main([])
            self.assertEqual(code, 1)
            self.assertIn("not found:", err.getvalue())
            self.assertIn(rel, err.getvalue())
        finally:
            path.unlink(missing_ok=True)
        self.assertNotIn(rel, self.m.collect_added("gate"))

    def test_staged_add_is_visible_to_cached_mode(self) -> None:
        rel = "tests/suite/widget_5226.aura"
        path = ROOT / rel
        self.assertFalse(path.exists())
        try:
            path.write_text(";; probe\n", encoding="utf-8")
            subprocess.run([*GIT, "add", "--", rel], cwd=ROOT, check=True)
            self.assertIn(rel, self.m.collect_added("cached"))
            self.assertIn(rel, self.m.banned_added("cached"))
            err = io.StringIO()
            with redirect_stderr(err):
                code = self.m.main(["--cached"])
            self.assertEqual(code, 1)
            self.assertIn("not found:", err.getvalue())
            self.assertIn(rel, err.getvalue())
        finally:
            subprocess.run([*GIT, "reset", "-q", "HEAD", "--", rel], cwd=ROOT, check=False)
            path.unlink(missing_ok=True)
        self.assertNotIn(rel, self.m.collect_added("cached"))

    def test_legal_untracked_theme_file_is_not_banned(self) -> None:
        rel = "tests/suite/parser_brand_new_theme.aura"
        path = ROOT / rel
        self.assertFalse(path.exists())
        try:
            path.write_text(";; probe\n", encoding="utf-8")
            self.assertIn(rel, self.m.collect_added("gate"))
            self.assertNotIn(rel, self.m.banned_added("gate"))
        finally:
            path.unlink(missing_ok=True)

    def test_modified_existing_numbered_file_is_not_added(self) -> None:
        rel = "tests/python/test_issue_5198_export_contracts.py"
        status = subprocess.run(
            [*GIT, "status", "--porcelain", "--", rel],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        if status.stdout.strip():
            self.skipTest(f"{rel} is already dirty")
        path = ROOT / rel
        original = path.read_bytes()
        try:
            with path.open("ab") as handle:
                handle.write(b"\n# probe\n")
            self.assertNotIn(rel, self.m.collect_added("gate"))
            self.assertNotIn(rel, self.m.collect_added("cached"))
            self.assertNotIn(rel, self.m.banned_added("gate"))
        finally:
            path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
