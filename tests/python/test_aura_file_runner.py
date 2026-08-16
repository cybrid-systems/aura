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
    discover_aura_files,
    first_expect,
    judge,
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


if __name__ == "__main__":
    unittest.main()
