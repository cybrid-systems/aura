#!/usr/bin/env python3
"""Issue #5234: --serve error semantics, stdout separation and JSON validity."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def serve(lines: list[str]) -> tuple[subprocess.CompletedProcess[str], list[dict]]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    p = subprocess.run(
        [str(AURA_BIN), "--serve"], input="\n".join(lines) + "\n", capture_output=True, text=True, env=env, timeout=120
    )
    replies = [json.loads(ln) for ln in p.stdout.splitlines() if ln.strip()]
    return p, replies


def serve_async(lines: list[str]) -> tuple[subprocess.CompletedProcess[str], list[dict]]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    p = subprocess.run(
        [str(AURA_BIN), "--serve-async"],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
    )
    replies = [json.loads(ln) for ln in p.stdout.splitlines() if ln.strip()]
    return p, replies


class Diag5234(unittest.TestCase):
    def test_display_of_error_value_reports_error(self) -> None:
        p, replies = serve(["(display (car 5))"])
        self.assertTrue(replies, p.stderr)
        self.assertEqual(replies[-1]["status"], "error", p.stdout)
        self.assertIn("error value was displayed", replies[-1]["msg"])

    def test_program_stdout_does_not_glue_json(self) -> None:
        p, replies = serve(['(display "still")', '(error "x")'])
        # Every stdout line parses as one JSON object (serve() would raise).
        self.assertEqual(len(replies), 2, p.stdout)
        self.assertEqual(replies[0]["status"], "ok")
        self.assertEqual(replies[1]["status"], "error")
        self.assertIn("still", p.stderr)
        self.assertNotIn("still{", p.stdout)

    def test_json_exec_tab_escape_is_valid_json(self) -> None:
        cmd = json.dumps({"cmd": "exec", "code": '(error "a\\tb")'})
        p, replies = serve([cmd])
        self.assertTrue(replies, p.stderr)
        self.assertEqual(replies[0]["status"], "error")
        self.assertIn("a\tb", replies[0]["msg"])

    def test_exec_lines_match_the_chunk(self) -> None:
        cmd = json.dumps({"cmd": "exec", "code": "(display 1)\n(display zz)"})
        p, replies = serve([cmd])
        self.assertTrue(replies, p.stderr)
        self.assertEqual(replies[0]["status"], "error")
        self.assertIn("2:10: unbound variable: zz", replies[0]["msg"], replies[0])

    def test_prelude_matches_file_mode(self) -> None:
        # The oneshot prelude loads std/hash; serve must resolve make-hash too
        # and report the same hash-ref failure instead of "unbound variable".
        cmd = json.dumps({"cmd": "exec", "code": '(display (hash-ref (make-hash) "k"))'})
        p, replies = serve([cmd])
        self.assertTrue(replies, p.stderr)
        self.assertEqual(replies[0]["status"], "error")
        self.assertNotIn("unbound variable", replies[0]["msg"])
        self.assertIn("hash-ref: key", p.stderr)

    def test_async_unbound_is_user_level(self) -> None:
        p, replies = serve_async(["(display zz)"])
        self.assertTrue(replies, p.stderr)
        self.assertEqual(replies[0]["status"], "error")
        self.assertNotIn("tree-walker", replies[0]["msg"])
        self.assertIn("unbound variable: zz", replies[0]["msg"])

    def test_async_displayed_error_is_failure(self) -> None:
        p, replies = serve_async(["(display (car 5))"])
        self.assertTrue(replies, p.stderr)
        self.assertEqual(replies[0]["status"], "error")
        self.assertIn("error value was displayed", replies[0]["msg"])

    def test_json_exec_display_error_reports_error(self) -> None:
        cmd = json.dumps({"cmd": "exec", "code": "(display (car 5))"})
        p, replies = serve([cmd])
        self.assertTrue(replies, p.stderr)
        self.assertEqual(replies[0]["status"], "error", p.stdout)


if __name__ == "__main__":
    unittest.main()
