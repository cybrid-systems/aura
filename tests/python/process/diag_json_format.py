#!/usr/bin/env python3
"""Issue #5240: --error-format=json emits one stable machine-readable diagnostic."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def run(args: list[str], cwd: str | None = None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    return subprocess.run([str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120)


def last_json(stderr: str) -> dict:
    lines = [ln for ln in stderr.splitlines() if ln.startswith("{")]
    assert lines, stderr
    return json.loads(lines[-1])


class DiagJson(unittest.TestCase):
    def test_parse_error_has_stable_code_and_span(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            p.write_text('(display "abc)\n')
            r = run(["--error-format=json", str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            obj = last_json(r.stderr)
            self.assertEqual(obj["code"], "ParseError")
            self.assertEqual(obj["file"], str(p))
            self.assertEqual(obj["span"]["start"], {"line": 1, "column": 10})
            self.assertIn("unterminated string literal", obj["message"])
            self.assertIn("abc", obj["snippet"])

    def test_runtime_error_code_and_position(self) -> None:
        r = run(["--error-format", "json", "-e", "(car 5)"])
        self.assertNotEqual(r.returncode, 0)
        obj = last_json(r.stderr)
        self.assertEqual(obj["code"], "TypeError")
        self.assertEqual(obj["span"]["start"], {"line": 1, "column": 2})
        self.assertIn("car: not a pair", obj["message"])
        self.assertIsInstance(obj["code"], str)

    def test_serve_error_uses_the_same_code_and_span(self) -> None:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        p = subprocess.run(
            [str(AURA_BIN), "--serve"], input="(car 5)\n", capture_output=True, text=True, env=env, timeout=120
        )
        replies = [json.loads(ln) for ln in p.stdout.splitlines() if ln.strip()]
        self.assertTrue(replies, p.stderr)
        obj = replies[-1]
        self.assertEqual(obj["status"], "error")
        self.assertEqual(obj["code"], "TypeError")
        self.assertEqual(obj["span"]["start"], {"line": 1, "column": 2})
        self.assertIn("car: not a pair", obj["message"])

    def test_serve_async_error_uses_the_same_code_and_span(self) -> None:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        p = subprocess.run(
            [str(AURA_BIN), "--serve-async"],
            input='(error "x")\n',
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        replies = [json.loads(ln) for ln in p.stdout.splitlines() if ln.strip()]
        self.assertTrue(replies, p.stderr)
        obj = replies[-1]
        self.assertEqual(obj["status"], "error")
        self.assertEqual(obj["code"], "UserError")
        self.assertEqual(obj["message"], "x")
        self.assertEqual(obj["span"]["start"], {"line": 1, "column": 2})

    def test_division_by_zero_has_its_own_code(self) -> None:
        # #5241 batch 1: division by zero is typed at creation, not TypeError.
        r = run(["--error-format=json", "-e", "(/ 1 0)"])
        self.assertNotEqual(r.returncode, 0)
        obj = last_json(r.stderr)
        self.assertEqual(obj["code"], "DivisionByZero")
        self.assertIn("division by zero", obj["message"])
        for src in ("(modulo 1 0)", "(quotient 1 0)", "(remainder 1 0)"):
            r = run(["--error-format=json", "-e", src])
            self.assertEqual(last_json(r.stderr)["code"], "DivisionByZero", src)

    def test_exit_code_table(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            p.write_text('(display "abc)\n')
            self.assertEqual(run([str(p)], cwd=d).returncode, 3)  # parse
        self.assertEqual(run(["-e", "(display zz)"]).returncode, 4)  # static/unbound
        self.assertEqual(run(["-e", "(car 5)"]).returncode, 1)  # runtime
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        empty = subprocess.run([str(AURA_BIN)], input="", capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(empty.returncode, 2)  # usage

    def test_default_text_output_is_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            p.write_text('(display "abc)\n')
            r = run([str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("error:", r.stderr)
            self.assertIn("^", r.stderr)
            self.assertNotIn('"code":', r.stderr)


if __name__ == "__main__":
    unittest.main()
