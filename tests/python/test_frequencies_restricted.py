#!/usr/bin/env python3
"""Issue #5188 and #5190: frequencies under the default Restricted preset.

hash-set! on a hash that has not been published to a workspace does not
need Mutate. A hash stored with define has been published: in-place
hash-set! is still denied. Requiring std/hash and std/iter in either
order keeps the hash shape (length 2, "a" -> 2).
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
    env.pop("AURA_SANDBOX", None)
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


class TestFrequenciesRestricted(unittest.TestCase):
    def test_hash_then_iter_is_a_hash(self) -> None:
        p = _run(
            '(require "std/hash" all:)\n'
            '(require "std/iter" all:)\n'
            '(define f (frequencies (list "a" "b" "a")))\n'
            '(display (hash? f)) (display " ")\n'
            '(display (pair? f)) (display " ")\n'
            '(display (hash-length f)) (display " ")\n'
            '(display (hash-ref f "a" 0)) (display " ")\n'
            '(display (hash-ref f "b" 0)) (newline)\n'
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("#t #f 2 2 1", p.stdout.replace("\n", " "))

    def test_iter_then_hash_is_a_hash(self) -> None:
        p = _run(
            '(require "std/iter" all:)\n'
            '(require "std/hash" all:)\n'
            '(define f (frequencies (list "a" "b" "a")))\n'
            '(display (hash? f)) (display " ")\n'
            '(display (hash-length f)) (display " ")\n'
            '(display (hash-ref f "a" 0)) (newline)\n'
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("#t 2 2", p.stdout.replace("\n", " "))

    def test_local_let_hash_set_and_published_still_denied(self) -> None:
        p = _run(
            '(require "std/hash" all:)\n'
            "(define (local-count)\n"
            '  (let ((h (hash))) (hash-set! h "a" 1) (hash-ref h "a" 0)))\n'
            "(display (local-count)) (newline)\n"
            '(define h (hash "a" 1))\n'
            '(define h2 (hash-set h "b" 2))\n'
            '(display (hash-length h)) (display " ") (display (hash-length h2)) (newline)\n'
            "(define denied (try (begin (hash-set! h \"c\" 3) 'ok) (catch (e) 'denied)))\n"
            "(display denied) (newline)\n"
            '(define f (frequencies (list "a" "b" "a")))\n'
            "(define later (try (begin (hash-set! f \"z\" 1) 'ok) (catch (e) 'denied)))\n"
            "(display later) (newline)\n"
            '(display (hash-ref f "a" 0)) (newline)\n'
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        lines = [ln.strip() for ln in p.stdout.splitlines() if ln.strip()]
        self.assertEqual(lines[0], "1")
        self.assertEqual(lines[1], "1 2")
        self.assertEqual(lines[2], "denied")
        self.assertEqual(lines[3], "denied")
        self.assertEqual(lines[4], "2")


if __name__ == "__main__":
    unittest.main()
