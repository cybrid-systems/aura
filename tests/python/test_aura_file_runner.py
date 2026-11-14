#!/usr/bin/env python3
"""Unit tests for the shared .aura file runner (no aura binary)."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from aura_file_runner import (  # noqa: E402
    AuraFileCase,
    CommandSpec,
    SnippetSpec,
    discover_aura_files,
    file_process_directives,
    first_expect,
    format_failure_detail,
    judge,
    judge_aura_file,
    judge_snippet,
    stdin_payload,
)


class TestAuraFileRunner(unittest.TestCase):
    def test_first_expect_takes_first_line(self) -> None:
        text = ";; a\n;; expect: 42\n(+ 1 1)\n;; expect: ignored\n"
        self.assertEqual(first_expect(text), "42")
        self.assertIsNone(first_expect("(display 1)\n"))

    def test_stdin_payload_strips_leading_header(self) -> None:
        text = ';; expect: ok\n;; note\n\n(display "ok")\n;; later\n'
        self.assertEqual(stdin_payload(text), '(display "ok")\n;; later')

    def test_judge_default_exit(self) -> None:
        self.assertTrue(judge(0, "ok", "", None)[0])
        self.assertFalse(judge(1, "", "boom", None)[0])
        self.assertFalse(judge(-11, "", "", None)[0])

    def test_judge_expect_variants(self) -> None:
        self.assertTrue(judge(0, "hello 42\n", "", "42")[0])
        self.assertFalse(judge(0, "nope", "", "42")[0])
        self.assertTrue(judge(0, "", "", "no-crash")[0])
        self.assertFalse(judge(-6, "", "", "no-crash")[0])
        self.assertTrue(judge(1, "", "user error", "no-error")[0])
        self.assertFalse(judge(0, "", "Internal Error: x", "no-error")[0])
        self.assertTrue(judge(2, "", "", "no-timeout")[0])

    def test_format_failure_detail_keeps_signal_stderr(self) -> None:
        # CI #4910: SIGABRT dropped === AURA CRASH === because only "exit" details
        # attached stderr. Signal deaths must keep the crash / FATAL tail.
        detail = format_failure_detail(
            "SIGABRT",
            stdout="  pass: fanout-plain returns 4\n",
            stderr="=== AURA CRASH: SIGABRT (signal 6) ip=0x1 ===\nFATAL: residual\n",
        )
        self.assertTrue(detail.startswith("SIGABRT:"))
        self.assertIn("AURA CRASH", detail)
        self.assertIn("FATAL", detail)
        self.assertEqual(format_failure_detail("", stdout="", stderr=""), "failed")
        self.assertEqual(format_failure_detail("exit 1", stdout="", stderr=""), "exit 1")

    def test_discover_skip_exclude_allow(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "keep.aura").write_text(";; expect: 1\n1\n", encoding="utf-8")
            (root / "skip_me.aura").write_text("1\n", encoding="utf-8")
            (root / "run-tests.aura").write_text("1\n", encoding="utf-8")
            (root / "other.aura").write_text("1\n", encoding="utf-8")
            found = discover_aura_files(
                root,
                skip={"skip_me.aura": "broken"},
                exclude={"run-tests.aura"},
                allow={"keep.aura", "skip_me.aura", "run-tests.aura"},
            )
            self.assertEqual([c.path.name for c in found.cases], ["keep.aura"])
            self.assertEqual(found.cases[0].expect, "1")
            self.assertEqual(len(found.skipped), 1)
            self.assertIn("skip_me.aura", found.skipped[0][0])

    def test_judge_snippet_integ_rules(self) -> None:
        spec = SnippetSpec(name="n", code="x", expect_out="42", expect_status=0)
        self.assertTrue(judge_snippet(0, "ans 42", "", spec)[0])
        self.assertFalse(judge_snippet(1, "ans 42", "", spec)[0])
        spec_ge = SnippetSpec(name="n", code="x", expect_out=">= 10")
        self.assertTrue(judge_snippet(0, "val 12", "", spec_ge)[0])
        self.assertFalse(judge_snippet(0, "val 3", "", spec_ge)[0])
        spec_err = SnippetSpec(name="n", code="x", expect_err="boom", expect_status=1)
        self.assertTrue(judge_snippet(1, "", "boom happened", spec_err)[0])
        spec_div = SnippetSpec(name="n", code="x", expect_status=0, accept_status=(0, -8, 1))
        self.assertTrue(judge_snippet(-8, "", "", spec_div)[0])

    def test_judge_snippet_p0_rules(self) -> None:
        spec = SnippetSpec(
            name="n",
            code="x",
            expect_out="(1 2)",
            expect_status=None,
            exact_out=True,
        )
        self.assertTrue(judge_snippet(0, "(1 2)", "", spec)[0])
        self.assertFalse(judge_snippet(0, "0 (1 2) extra", "", spec)[0])
        spec_re = SnippetSpec(
            name="n",
            code="x",
            expect_err="div(ide)?",
            expect_status=None,
            err_regex=True,
        )
        self.assertTrue(judge_snippet(1, "", "divide by zero", spec_re)[0])
        self.assertFalse(judge_snippet(1, "", "oops", spec_re)[0])
        self.assertFalse(judge_snippet(0, "", "", spec, timed_out=True)[0])

    def test_judge_snippet_type_line(self) -> None:
        spec = SnippetSpec(
            name="n",
            code="x",
            expect_out="Int",
            expect_status=None,
            type_line=True,
        )
        self.assertTrue(judge_snippet(0, "ok\ntype: Int\n", "", spec)[0])
        self.assertFalse(judge_snippet(0, "Int without prefix\n", "", spec)[0])
        self.assertFalse(judge_snippet(0, "type: Float\n", "", spec)[0])

    def test_command_spec_shape(self) -> None:
        spec = CommandSpec(name="basic", command="echo 3", expect="3")
        self.assertEqual(spec.timeout_s, 30)

    def test_file_process_directives(self) -> None:
        text = ';; expect-status: 1\n;; expect-err: demo-stderr-5224\n(error "x")\n'
        self.assertEqual(file_process_directives(text), (1, "demo-stderr-5224", False))
        regex = ";; expect-err-regex: div(ide)?\n"
        self.assertEqual(file_process_directives(regex), (None, "div(ide)?", True))
        self.assertEqual(file_process_directives(";; expect: 42\n"), (None, "", False))

    def test_judge_aura_file_snippet_status(self) -> None:
        miss = AuraFileCase(
            path=Path("x.aura"),
            expect=None,
            rel="suite/x.aura",
            expect_status=1,
            expect_err="demo-stderr-5224",
        )
        self.assertFalse(judge_aura_file(0, "", "demo-stderr-5224", miss)[0])
        self.assertFalse(judge_aura_file(1, "", "other", miss)[0])
        self.assertTrue(judge_aura_file(1, "", "error: demo-stderr-5224\n", miss)[0])
        legacy = AuraFileCase(path=Path("y.aura"), expect=None, rel="suite/y.aura")
        self.assertFalse(judge_aura_file(1, "", "", legacy)[0])
        self.assertTrue(judge_aura_file(0, "", "", legacy)[0])

    def test_discover_reads_process_directives(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "demo.aura").write_text(
                ';; expect-status: 1\n;; expect-err: needle\n(error "needle")\n',
                encoding="utf-8",
            )
            found = discover_aura_files(root)
            self.assertEqual(len(found.cases), 1)
            case = found.cases[0]
            self.assertEqual(case.expect_status, 1)
            self.assertEqual(case.expect_err, "needle")
            self.assertFalse(case.err_regex)


if __name__ == "__main__":
    unittest.main()
