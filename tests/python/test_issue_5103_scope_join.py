#!/usr/bin/env python3
"""Regression for #5103: a settled scope-join does not destroy the scope.

orch:scope-join-all used to drop the per-Evaluator scope once the tree
was settled, so the next join, watch, or cancel raised "no scope".
The scope stays registered. A later join returns a status hash, watch
returns a status, and a fresh scope-spawn still works.
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
(orch:scope-spawn "a" (lambda () (fiber:yield)))
(orch:scope-spawn "b" (lambda () (fiber:yield)))
(define c (orch:scope-cancel-all))
(define j1 (orch:scope-join-all :timeout-ms 200))
(define j2 (orch:scope-join-all :timeout-ms 200))
(define w (orch:scope-watch))
(define s (orch:scope-spawn "c" (lambda () 1)))
(define j3 (orch:scope-join-all :timeout-ms 200))
(display (format "cancel=~a j1=~a j2=~a watch=~a spawn=~a j3=~a\n"
                 (hash-ref c "status" "?")
                 (hash-ref j1 "status" "?")
                 (hash-ref j2 "status" "?")
                 (if (hash? w) "status" "bad")
                 (hash-ref s "ok" #f)
                 (hash-ref j3 "status" "?")))
"""


def run_prog(src: str) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    env.pop("AURA_PIPELINE_STRICT", None)
    env.pop("AURA_PRESET", None)
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
            stdin=subprocess.DEVNULL,
        )
    finally:
        os.unlink(path)


class ScopeJoinKeepsScope(unittest.TestCase):
    def test_second_join_and_watch_do_not_raise(self) -> None:
        proc = run_prog(PROG)
        out = (proc.stdout or "") + (proc.stderr or "")
        self.assertEqual(proc.returncode, 0, out)
        self.assertNotIn("no scope", out)
        self.assertIn("j1=ok", proc.stdout or "")
        self.assertIn("j2=ok", proc.stdout or "")
        self.assertIn("watch=status", proc.stdout or "")
        self.assertIn("spawn=#t", proc.stdout or "")
        self.assertIn("j3=ok", proc.stdout or "")


if __name__ == "__main__":
    unittest.main()
