#!/usr/bin/env python3
"""Regression for #5162: evo:trial must not freeze the parent workspace.

Under --preset dev, mutate:rebind succeeds before a trial. A successful
trial (apply returns #t, evaluate returns only a hash) leaves workspace 0
current and the live source unchanged. The next mutate:rebind must succeed.
set-code still installs defines, and a rebind after that set-code succeeds
too. A child COW inside a second trial must not stick the parent on the
read-only layer. AURA_SANDBOX=off hides this, so the test leaves it unset.
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

PROG = """
(require "std/evo-core" all:)
(set-code "(define (score x) (* x 2))")
(eval-current)
(define (show tag v)
  (display tag) (display " ") (display v) (newline))
(show "BEFORE" (mutate:rebind "score" "(lambda (x) (* x 3))" "before"))
(eval-current)
(define cand (evo:candidate "(lambda (x) (* x 9))" (hash "id" "c1" "target" "score")))
(define noop
  (evo:trial cand
             (hash "apply" (lambda (c) #t)
                   "evaluate" (lambda (c)
                                (hash "objectives" (hash "y" 1)
                                      "constraints" (hash)
                                      "behavior" (hash))))))
(show "NOOP-STATUS" (hash-ref noop "status"))
(show "NOOP-OK" (hash-ref noop "ok"))
(show "NOOP-LIVE" (hash-ref noop "live-unchanged"))
(show "NOOP-ROOT" (hash-ref noop "root"))
(show "NOOP-WS" (workspace :current))
(show "NOOP-ERR" (hash-ref noop "error" #f))
(define cow
  (evo:trial cand
             (hash "apply" (lambda (c)
                             (eq? (mutate:rebind "score"
                                                (evo:candidate-genome c)
                                                "child") #t))
                   "evaluate" (lambda (c)
                                (hash "objectives" (hash "y" (score 5))
                                      "constraints" (hash)
                                      "behavior" (hash))))))
(show "COW-STATUS" (hash-ref cow "status"))
(show "COW-OK" (hash-ref cow "ok"))
(show "COW-Y" (evo:objective cow "y"))
(show "COW-LIVE" (hash-ref cow "live-unchanged"))
(show "COW-WS" (workspace :current))
(show "PARENT" (score 5))
(show "AFTER" (mutate:rebind "score" "(lambda (x) (* x 4))" "after"))
(eval-current)
(show "AFTER-SCORE" (score 5))
(show "AFTER-WS" (workspace :current))
(set-code "(define (score x) (* x 7))")
(eval-current)
(show "SET" (score 5))
(show "AFTER-SET" (mutate:rebind "score" "(lambda (x) (* x 8))" "after-set"))
(eval-current)
(show "SET-SCORE" (score 5))
(show "SET-WS" (workspace :current))
"""


def env(log_path: Path) -> dict[str, str]:
    out = dict(os.environ)
    # The refuse only shows while the sandbox stays Restricted.
    out.pop("AURA_SANDBOX", None)
    out.pop("AURA_PRESET", None)
    out.pop("AURA_PIPELINE_STRICT", None)
    out["AURA_EVO_LOG"] = str(log_path)
    return out


class TrialParentRebind(unittest.TestCase):
    def test_preset_dev_parent_rebind_after_trial(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "p.aura").write_text(PROG)
            r = subprocess.run(
                [str(AURA_BIN), "--preset", "dev", "--quiet", "p.aura"],
                cwd=d,
                capture_output=True,
                text=True,
                env=env(root / "evo.jsonl"),
                timeout=120,
            )
            blob = r.stdout + r.stderr
            self.assertEqual(r.returncode, 0, blob)
            self.assertNotIn("cow-refused", blob)
            for line in (
                "BEFORE #t",
                "NOOP-STATUS ok",
                "NOOP-OK #t",
                "NOOP-LIVE #t",
                "NOOP-ROOT 0",
                "NOOP-WS 0",
                "NOOP-ERR #f",
                "COW-STATUS ok",
                "COW-OK #t",
                "COW-Y 45",
                "COW-LIVE #t",
                "COW-WS 0",
                "PARENT 15",
                "AFTER #t",
                "AFTER-SCORE 20",
                "AFTER-WS 0",
                "SET 35",
                "AFTER-SET #t",
                "SET-SCORE 40",
                "SET-WS 0",
            ):
                self.assertIn(line, r.stdout, blob)


if __name__ == "__main__":
    unittest.main()
