#!/usr/bin/env python3
"""Regression for #5086: preset=dev must mutate a child workspace.

Workspace 0 mutate:rebind succeeds under --preset dev, but evo:trial's
child used to return cow-refused unless AURA_SANDBOX=off. The child
rewrite stays off the live worldline.
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

PROG = """
(require "std/evo-core" all:)
(set-code "(define (score x) (* x 2))")
(eval-current)
(define root-rb (mutate:rebind "score" "(lambda (x) (* x 3))" "root"))
(eval-current)
(define c (evo:candidate "(lambda (x) (* x 9))" (hash "target" "score")))
(define t (evo:trial c
                     (hash "apply" (lambda (cand)
                                     (eq? (mutate:rebind "score"
                                                         (evo:candidate-genome cand)
                                                         "child") #t))
                           "evaluate" (lambda (cand)
                                        (hash "objectives" (hash "y" (score 5))
                                              "constraints" (hash)
                                              "behavior" (hash))))))
(display (list "root" root-rb
               "status" (hash-ref t "status")
               "err" (hash-ref t "error" #f)
               "y" (evo:objective t "y")
               "live" (score 5)
               "unchanged" (hash-ref t "live-unchanged")))
(newline)
"""


def env() -> dict[str, str]:
    out = dict(os.environ)
    # The bug only shows while the sandbox stays Restricted.
    out.pop("AURA_SANDBOX", None)
    out.pop("AURA_PIPELINE_STRICT", None)
    return out


class DevChildCow(unittest.TestCase):
    def test_preset_dev_trial_mutates_child_not_live(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(PROG)
            r = subprocess.run(
                [str(AURA_BIN), "--preset", "dev", "--quiet", "p.aura"],
                cwd=d,
                capture_output=True,
                text=True,
                env=env(),
                timeout=120,
            )
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            line = r.stdout.strip().splitlines()[-1]
            self.assertNotIn("cow-refused", r.stdout + r.stderr, line)
            self.assertIn("root #t", line)
            self.assertIn("status ok", line)
            self.assertIn("y 45", line)
            self.assertIn("live 15", line)
            self.assertIn("unchanged #t", line)


if __name__ == "__main__":
    unittest.main()
