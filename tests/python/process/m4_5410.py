"""Issue #5410: live-process worldline fork / snapshot API contract.

Snapshot is a code checkpoint of the live workspace; a file/-e top-level
program has no workspace and correctly returns -1.  The M4 fork primitive is
the per-fiber COW overlay (mutate :overlay-*), not ast:snapshot.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(ROOT / "tests" / "python"))
from _aura_harness import AURA_BIN  # noqa: E402


def run(program: str) -> dict[str, str]:
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "probe.aura"
        p.write_text(program)
        env = os.environ.copy()
        env["AURA_SANDBOX"] = "off"
        env["AURA_PIPELINE_STRICT"] = "0"
        r = subprocess.run([str(AURA_BIN), str(p)], capture_output=True, text=True, timeout=120, env=env)
        if r.returncode != 0:
            raise AssertionError(f"aura rc={r.returncode}\n{r.stdout}\n{r.stderr}")
        out: dict[str, str] = {}
        for line in r.stdout.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k] = v
        return out


PROBE = r"""
(define (p k v) (display k) (display "=") (display v) (newline))
(p "pre_snapshot" (mutate :snapshot "n"))
(p "pre_reason" (query :snapshot-fail-reason))
(p "ast_snapshot" (try (ast:snapshot "a") (catch (e) "unbound")))
(set-code "(define x 10)")
(eval-current)
(p "post_snapshot" (mutate :snapshot "s"))
(p "mw_snapshot" (try (mw:snapshot "m") (catch (e) -2)))
(define f1 (fiber:spawn (lambda ()
  (mutate :overlay-begin)
  (mutate :overlay-set! "x" 11)
  (mutate :overlay-commit))))
(define f2 (fiber:spawn (lambda ()
  (mutate :overlay-begin)
  (mutate :overlay-set! "x" 99)
  (mutate :overlay-rollback))))
(p "join1" (fiber:join f1))
(p "join2" (fiber:join f2))
(p "x" x)
"""


class Fork(unittest.TestCase):
    def test_snapshot_contract(self) -> None:
        out = run(PROBE)
        self.assertEqual(out["pre_snapshot"], "-1")
        self.assertEqual(out["pre_reason"], ":no-workspace")
        self.assertEqual(out["ast_snapshot"], "unbound")
        self.assertGreaterEqual(int(out["post_snapshot"]), 0)
        self.assertGreaterEqual(int(out["mw_snapshot"]), 0)

    def test_overlay_is_the_fork_api(self) -> None:
        out = run(PROBE)
        self.assertEqual(out["x"], "11")  # f1 committed; f2 rolled back
