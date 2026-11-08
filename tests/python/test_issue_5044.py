#!/usr/bin/env python3
"""Regression for #5044: :ask feeds a model answer through :propose."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN, repl_record  # noqa: E402

BASE = '(set-code "(define (guess) 0)")\n:focus guess\n'


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH", "AURA_REPL_RECORD"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    return e


def run_repl(stdin: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(AURA_BIN), "repl", "--quiet"],
        input=stdin,
        capture_output=True,
        text=True,
        env=env(),
        timeout=120,
    )


def fixture_chat(content: str) -> str:
    return f'(define (llm:chat s u) (hash "ok" #t "content" "{content}"))\n'


STUB = '(define (extract-code r) (hash-ref r "content"))\n'


class Ask(unittest.TestCase):
    def test_better_definition_is_kept(self) -> None:
        r = run_repl(BASE + fixture_chat("(define (guess) 42)") + STUB + ":ask 改到 42\n(quit)\n")
        self.assertIn("candidate", r.stdout, r.stdout + r.stderr)
        recs = [repl_record(x) for x in r.stdout.splitlines() if x.startswith("(hash")]
        keeper = [x for x in recs if x.get("decision")]
        self.assertTrue(keeper, r.stdout)
        self.assertEqual(keeper[-1].get("decision"), "KEEP", keeper[-1])
        self.assertEqual(keeper[-1].get("candidate"), "(define (guess) 42)", keeper[-1])
        self.assertIn("(lambda () 42)", keeper[-1].get("source", ""), keeper[-1])

    def test_model_failure_does_not_rewrite(self) -> None:
        r = run_repl(
            BASE
            + '(define (llm:chat s u) (hash "ok" #f "error" "injection-blocked" "content" ""))\n'
            + STUB
            + ":ask 改到 42\n:src guess\n(quit)\n"
        )
        self.assertIn("injection-blocked", r.stdout, r.stdout + r.stderr)
        self.assertNotIn("KEEP", r.stdout)
        self.assertIn("(define guess (lambda () 0))", r.stdout)

    def test_extracted_nothing_does_not_rewrite(self) -> None:
        r = run_repl(
            BASE
            + fixture_chat("no code here")
            + '(define (extract-code r) "")\n'
            + ":ask 改到 42\n:src guess\n(quit)\n"
        )
        self.assertIn("no definition extracted", r.stdout, r.stdout + r.stderr)
        self.assertNotIn("KEEP", r.stdout)
        self.assertIn("(define guess (lambda () 0))", r.stdout)


if __name__ == "__main__":
    unittest.main()
