#!/usr/bin/env python3
"""Regression for #5164: mutate node ops must not SIGSEGV with no workspace.

Under --preset dev, (mutate:record-patch 0 "op" "sum") and
(mutate:rollback-macro-introduced 0) used to dereference a null FlatAST
inside FlatAST::size() and die with SIGSEGV. Both now return a catchable
no-workspace error, including a string node argument. After set-code the
same calls are no longer no-workspace. AURA_SANDBOX=off hides the
Restricted face, so the test leaves it unset.
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

PROG = r"""
(define (show tag thunk)
  (let ((r (try (thunk) (catch e e))))
    (display tag)
    (display " ")
    (display (if (pair? r) (car r) r))
    (newline)))
(show "PATCH" (lambda () (mutate:record-patch 0 "op" "sum")))
(show "ROLL" (lambda () (mutate:rollback-macro-introduced 0)))
(show "STR" (lambda () (mutate:rollback-macro-introduced "x")))
(set-code "(define (score x) x)")
(eval-current)
(show "AFTER-PATCH" (lambda () (mutate:record-patch 0 "op" "sum")))
(show "AFTER-ROLL" (lambda () (mutate:rollback-macro-introduced 0)))
"""


class TestIssue5164MutateNoWorkspace(unittest.TestCase):
    def test_numeric_node_without_workspace_is_no_workspace(self) -> None:
        env = os.environ.copy()
        for key in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
            env.pop(key, None)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p.aura"
            path.write_text(PROG, encoding="utf-8")
            proc = subprocess.run(
                [str(AURA_BIN), "--preset", "dev", "--quiet", str(path)],
                cwd=tmp,
                env=env,
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
        out = proc.stdout
        err = proc.stderr
        self.assertEqual(proc.returncode, 0, f"rc={proc.returncode}\nstdout={out!r}\nstderr={err!r}")
        self.assertNotIn("AURA CRASH", err)
        self.assertNotIn("SIGSEGV", err)
        self.assertIn("PATCH no-workspace", out)
        self.assertIn("ROLL no-workspace", out)
        self.assertIn("STR no-workspace", out)
        self.assertNotIn("AFTER-PATCH no-workspace", out)
        self.assertNotIn("AFTER-ROLL no-workspace", out)


if __name__ == "__main__":
    unittest.main()
