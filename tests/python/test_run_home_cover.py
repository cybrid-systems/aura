#!/usr/bin/env python3
"""Query tests for tests/run.py home and cover (#5225). No Aura binary."""

from __future__ import annotations

import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from homes import (  # noqa: E402
    HOMES,
    CaseRec,
    FileRec,
    render_cover,
    render_home,
    resolve_homes,
    scan_tree,
    tokens,
)
from run import main  # noqa: E402


class TestHome(unittest.TestCase):
    def test_acceptance_queries(self) -> None:
        expect = {
            "string-join": "tests/suite/string_join.aura",
            "iota": "tests/suite/list_srfi_5133.aura",
            "unclosed list": "tests/suite/paren_semi_comment_5199.aura",
            "(;": "tests/suite/paren_semi_comment_5199.aura",
            "extract-code": "tests/suite/export_contracts_5198.aura",
            "fiber cancel": "tests/suite/issue_5064_fiber_cancel_text.aura",
            "--help": "tests/python/test_issue_4935_help.py",
        }
        for query, path in expect.items():
            code, text = render_home(query)
            self.assertEqual(code, 0, query)
            self.assertIn(f"home: {path}", text)
            self.assertNotIn("not found:", text)
            self.assertIn("(test-case", text)
            self.assertIn("(test-finish)", text)
            self.assertIn("(hash", text)
            self.assertIn("tests/suite/<module>_<feature>.aura", text)
            self.assertIn("lib/std/tests/", text)

    def test_iota_is_the_list_contract(self) -> None:
        code, text = render_home("iota")
        self.assertEqual(code, 0)
        self.assertIn("surface: std/list", text)
        self.assertIn("std/iter", text)
        self.assertIn("(check= (list 0 1 2) (iota 3))", text)
        self.assertIn("already uses test-case", text)

    def test_iter_and_algorithm_reexports(self) -> None:
        code, text = render_home("std/iter")
        self.assertEqual(code, 0)
        self.assertIn("tests/suite/list_srfi_5133.aura", text)
        code, text = render_home("std/algorithm")
        self.assertEqual(code, 0)
        self.assertIn("tests/suite/sort_srfi_5133.aura", text)

    def test_help_is_a_process_home(self) -> None:
        code, text = render_home("--help")
        self.assertEqual(code, 0)
        self.assertIn("language: python", text)
        self.assertIn("Behavior assertions do not go in this file.", text)
        self.assertNotIn("home: tests/suite/", text)

    def test_parser_diagnostic_snippet(self) -> None:
        _, text = render_home("unclosed list")
        self.assertIn(";; expect-err: unclosed list opened at line 1", text)
        self.assertIn("(;", text)

    def test_issue_number_lands_in_the_hash(self) -> None:
        code, text = render_home("iota #5198")
        self.assertEqual(code, 0)
        self.assertIn('(hash "issue" 5198 "tags" (list "list"))', text)
        self.assertIn("tests/suite/list_srfi_5133.aura", text)

    def test_miss_does_not_guess(self) -> None:
        code, text = render_home("no-such-surface-zz")
        self.assertEqual(code, 1)
        self.assertIn('not found: no home for "no-such-surface-zz".', text)
        self.assertIn("Do not guess a file.", text)
        self.assertNotIn("list_srfi", text)
        self.assertNotIn("string_join.aura", text)
        self.assertNotIn("home:", text)
        self.assertIn("tests/suite/<module>_<feature>.aura", text)

    def test_tie_does_not_guess(self) -> None:
        code, text = render_home("vector sort")
        self.assertEqual(code, 1)
        self.assertIn("matches more than one home", text)
        self.assertIn("Do not guess a file.", text)
        self.assertIn("tests/suite/vector_srfi_5133.aura", text)
        self.assertIn("tests/suite/sort_srfi_5133.aura", text)
        self.assertNotIn("home:", text)

    def test_cpp_theme_still_resolves(self) -> None:
        code, text = render_home("arena")
        self.assertEqual(code, 0)
        self.assertIn("home: tests/core/test_arena_batch.cpp", text)
        code, text = render_home("capability sandbox")
        self.assertEqual(code, 0)
        self.assertIn("home: tests/core/test_capability_sandbox_batch.cpp", text)
        code, text = render_home("sandbox")
        self.assertEqual(code, 0)
        self.assertIn("home: tests/python/test_issue_4870_production_sandbox.py", text)

    def test_keywords_do_not_share_a_surface(self) -> None:
        owners: dict[tuple[str, ...], set[str]] = {}
        for home in HOMES:
            for keyword in home.keywords:
                owners.setdefault(tokens(keyword), set()).add(home.surface)
        dupes = {key: val for key, val in owners.items() if len(val) > 1}
        self.assertEqual(dupes, {})

    def test_catalog_files_exist_and_are_in_the_genealogy(self) -> None:
        genealogy = (ROOT / "tests" / "HOMES.md").read_text(encoding="utf-8")
        self.assertIn("## Aura L1 homes", genealogy)
        self.assertIn("## Python process homes", genealogy)
        self.assertIn("## C++ query homes", genealogy)
        self.assertIn("## Preferred homes (theme → file)", genealogy)
        self.assertIn("lib/std/tests/", genealogy)
        self.assertIn("test_linear_cross_closure.cpp", genealogy)
        self.assertIn("tests/issues/test_issue_N.cpp", genealogy)
        for home in HOMES:
            self.assertTrue((ROOT / home.path).is_file(), home.path)
            self.assertIn(home.path, genealogy)
        self.assertEqual(len(resolve_homes("iota")), 1)


class TestCover(unittest.TestCase):
    def test_issue_rows_come_from_sources(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "demo.aura").write_text(
                "; Issue #4242\n"
                '(test-case "zymbol case" (hash "issue" 4242 "tags" (list "z"))\n'
                "  (check #t))\n"
                '(check "zymbol probe" 1 1)\n',
                encoding="utf-8",
            )
            files = scan_tree(root, rel_to=root)
            code, text = render_cover(issue=4242, files=files)
            self.assertEqual(code, 0, text)
            self.assertIn("demo.aura  zymbol case  #4242", text)
            self.assertIn("demo.aura  zymbol probe  #4242", text)
            code, named = render_cover(name="zymbol", files=files)
            self.assertEqual(code, 0, named)
            self.assertIn("zymbol case", named)
            self.assertIn("zymbol probe", named)

    def test_cover_issue_acceptance(self) -> None:
        code, text = render_cover(issue=5198)
        self.assertEqual(code, 0, text)
        self.assertIn("tests/suite/export_contracts_5198.aura", text)
        self.assertIn("iota iter-then-list", text)
        self.assertIn("tests/python/test_issue_5198_export_contracts.py", text)
        self.assertIn("#5198", text)

        code, text = render_cover(issue=5199)
        self.assertEqual(code, 0, text)
        self.assertIn("tests/suite/paren_semi_comment_5199.aura", text)
        self.assertIn("tests/python/test_issue_5199_unclosed_list.py", text)
        self.assertIn("#5199", text)

        code, text = render_cover(issue=5185)
        self.assertEqual(code, 0, text)
        self.assertIn("tests/suite/string_join.aura", text)
        self.assertIn("#5185", text)
        self.assertNotIn("lib/std/", text)

    def test_cover_name_iota_is_the_list_contract(self) -> None:
        code, text = render_cover(name="iota")
        self.assertEqual(code, 0, text)
        self.assertIn("home  std/list  tests/suite/list_srfi_5133.aura", text)
        self.assertIn("std/iter", text)
        self.assertIn("tests/suite/list_srfi_5133.aura", text)
        self.assertIn("iota iter-then-list", text)
        self.assertIn("test_iota_list_then_iter", text)
        self.assertNotIn("std::iota", text)
        self.assertNotIn("test_pcv", text)
        self.assertNotIn("algorithm_native_sort", text)
        self.assertNotIn("cow_set_after_commit", text)

    def test_missing_coverage(self) -> None:
        code, text = render_cover(name="zymbol-not-in-tree")
        self.assertEqual(code, 1)
        self.assertIn("no coverage", text)
        code, text = render_cover(issue=9)
        self.assertEqual(code, 1)
        self.assertIn("no coverage for issue 9.", text)

    def test_cli_entrypoint(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "run.py"), "home", "string-join"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("tests/suite/string_join.aura", proc.stdout)
        self.assertIn("(test-case", proc.stdout)
        buf = io.StringIO()
        with redirect_stdout(buf):
            joined = main(["home", "fiber", "cancel"])
        self.assertEqual(joined, 0)
        self.assertIn("tests/suite/issue_5064_fiber_cancel_text.aura", buf.getvalue())

    def test_query_sources_do_not_build(self) -> None:
        src = (ROOT / "tests" / "python" / "homes.py").read_text(encoding="utf-8")
        self.assertNotIn("subprocess", src)
        self.assertNotIn("ninja", src)
        self.assertNotIn("AURA_BIN", src)


class TestScanShape(unittest.TestCase):
    def test_hash_issue_is_per_case(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "one.aura").write_text(
                '(test-case "kept" (hash "issue" 4242 "tags" (list "a"))\n  (check #t))\n'
                '(test-case "plain" (hash)\n  (check #t))\n',
                encoding="utf-8",
            )
            files = scan_tree(root, rel_to=root)
            self.assertEqual(len(files), 1)
            by_name = {case.name: case for case in files[0].cases}
            self.assertEqual(by_name["kept"], CaseRec("kept", "test-case", (4242,)))
            self.assertEqual(by_name["plain"].hash_issues, ())
            self.assertIsInstance(files[0], FileRec)


if __name__ == "__main__":
    unittest.main()
