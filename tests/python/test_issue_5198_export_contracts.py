#!/usr/bin/env python3
"""Issue #5198: one contract per shared stdlib name, and a require-overwrite warning.

iota / sorted? / vector-map succeed in either require order. extract-code
stays the text extractor; llm:extract-code is the agent extractor. Requiring
two modules that still define find independently warns and still loads.
Requiring one module twice does not warn.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def _run(source: str) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_PATH"] = str(ROOT / "lib")
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    try:
        return subprocess.run(
            [str(AURA_BIN), path],
            capture_output=True,
            text=True,
            env=env,
            timeout=90,
        )
    finally:
        os.unlink(path)


class TestExportContracts5198(unittest.TestCase):
    def test_iota_list_then_iter(self) -> None:
        p = _run(
            '(require "std/list" all:)\n'
            '(require "std/iter" all:)\n'
            "(display (iota 5 1)) (newline)\n"
            "(display (iota 5)) (newline)\n"
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.splitlines(), ["(1 2 3 4 5)", "(0 1 2 3 4)"])
        self.assertNotIn("iota was already defined", p.stderr)

    def test_iota_iter_then_list(self) -> None:
        p = _run('(require "std/iter" all:)\n(require "std/list" all:)\n(display (iota 5 1)) (newline)\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), "(1 2 3 4 5)")
        self.assertNotIn("iota was already defined", p.stderr)

    def test_sorted_both_orders(self) -> None:
        body = (
            "(display (sorted? (list 1 2 3))) (newline)\n"
            "(display (sorted? (list 1 2 3) <)) (newline)\n"
            "(display (sorted? (list 3 1) >)) (newline)\n"
            "(display (sorted? (list 3 1))) (newline)\n"
        )
        for first, second in (
            ("std/sort", "std/algorithm"),
            ("std/algorithm", "std/sort"),
        ):
            p = _run(f'(require "{first}" all:)\n(require "{second}" all:)\n' + body)
            self.assertEqual(p.returncode, 0, f"{first} then {second}: {p.stderr}")
            self.assertEqual(
                p.stdout.splitlines(),
                ["#t", "#t", "#t", "#f"],
                f"{first} then {second}: {p.stdout!r} {p.stderr!r}",
            )
            self.assertNotIn("sorted? was already defined", p.stderr)

    def test_vector_map_both_orders(self) -> None:
        # The issue repro: iter's old vector-map rejected the third argument.
        repro = (
            "(display (vector-map (lambda (x) x) (make-vector 2 0) (make-vector 3 1)))\n"
            "(newline)\n"
            "(display (vector-map + (vector 1 2) (vector 10 20 30))) (newline)\n"
        )
        for first, second in (
            ("std/list", "std/iter"),
            ("std/iter", "std/list"),
            ("std/vector", "std/iter"),
            ("std/iter", "std/vector"),
        ):
            p = _run(f'(require "{first}" all:)\n(require "{second}" all:)\n' + repro)
            self.assertEqual(p.returncode, 0, f"{first} then {second}: {p.stderr}")
            self.assertEqual(
                p.stdout.splitlines(),
                ["#(() ())", "#(11 22)"],
                f"{first} then {second}: {p.stdout!r} {p.stderr!r}",
            )
            self.assertNotIn("arity mismatch", p.stderr)
            self.assertNotIn("vector-map was already defined", p.stderr)

    def test_extract_code_both_orders(self) -> None:
        body = (
            '(display (extract-code "x")) (newline)\n'
            '(define plain (try (llm:extract-code "x") (catch (e) \'llm-rejected-plain)))\n'
            "(display plain) (newline)\n"
            '(define fenced "{\\"error\\":false,\\"choices\\":[{\\"message\\":{\\"content\\":\\"(lambda (x) x)\\"}}]}")\n'
            "(display (llm:extract-code fenced)) (newline)\n"
        )
        for first, second in (("std/extract", "std/agent"), ("std/agent", "std/extract")):
            p = _run(f'(require "{first}" all:)\n(require "{second}" all:)\n' + body)
            self.assertEqual(p.returncode, 0, f"{first} then {second}: {p.stderr}")
            self.assertEqual(
                p.stdout.splitlines(),
                ["x", "llm-rejected-plain", "(lambda (x) x)"],
                f"{first} then {second}: {p.stdout!r} {p.stderr!r}",
            )
            self.assertNotIn("json-parse", p.stderr)
            self.assertNotIn("extract-code was already defined", p.stderr)

    def test_require_overwrite_warns_and_still_loads(self) -> None:
        p = _run(
            '(require "std/list" all:)\n'
            '(require "std/iter" all:)\n'
            "(display (find (lambda (x) (= x 2)) (list 1 2 3))) (newline)\n"
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("2", p.stdout)
        self.assertIn(
            "find was already defined by std/list; std/iter's definition will overwrite it.",
            p.stderr,
        )
        self.assertIn("warning:", p.stderr)

    def test_require_same_module_twice_does_not_warn(self) -> None:
        p = _run('(require "std/list" all:)\n(require "std/list" all:)\n(display (iota 5 1)) (newline)\n')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), "(1 2 3 4 5)")
        self.assertNotIn("will overwrite", p.stderr)
        self.assertNotIn("warning:", p.stderr)


if __name__ == "__main__":
    unittest.main()
