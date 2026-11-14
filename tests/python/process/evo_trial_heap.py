#!/usr/bin/env python3
"""Regression for #5163: evo:trial child may update local data.

Under --preset dev, a successful trial used to refuse hash-set!,
vector-set!, and set-car! with "mutate not granted" even though the
session held Mutate. Those slot updates succeed in the child. set-code
on that child stays denied, and a child mutate:rebind does not change
the parent worldline. AURA_SANDBOX=off hides the deny, so the test
leaves it unset.
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
(define ph (hash "k" 1))
(hash-set! ph "k" 2)
(show "PARENT-HASH" (hash-ref ph "k"))
(define cand (evo:candidate "(lambda (x) (* x 9))" (hash "id" "c1" "target" "score")))
(define (trial tag ev)
  (define r (evo:trial cand (hash "apply" (lambda (c) #t) "evaluate" ev)))
  (show (string-append tag "-STATUS") (hash-ref r "status"))
  (show (string-append tag "-ERR") (hash-ref r "error" #f))
  (show (string-append tag "-Y") (evo:objective r "y"))
  (show (string-append tag "-LIVE") (hash-ref r "live-unchanged"))
  (show (string-append tag "-WS") (workspace :current)))
(define (ok y)
  (hash "objectives" (hash "y" y) "constraints" (hash) "behavior" (hash)))
(trial "PURE" (lambda (c) (ok 1)))
(trial "SET" (lambda (c)
               (define n 1)
               (set! n 4)
               (ok n)))
(trial "REF" (lambda (c)
               (define h (hash "k" 3))
               (ok (hash-ref h "k"))))
(trial "HSET" (lambda (c)
                (define h (hash "k" 1))
                (hash-set! h "k" 9)
                (ok (hash-ref h "k"))))
(trial "VSET" (lambda (c)
                (define v (vector 1 2 3))
                (vector-set! v 1 8)
                (ok (vector-ref v 1))))
(trial "CAR" (lambda (c)
               (define p (cons 1 2))
               (set-car! p 6)
               (ok (car p))))
(trial "HREM" (lambda (c)
                (define h (hash "k" 1))
                (hash-remove! h "k")
                (ok (if (hash-has-key? h "k") 1 0))))
(trial "FILL" (lambda (c)
                (define v (vector 1 2 3))
                (vector-fill! v 4)
                (ok (vector-ref v 2))))
(trial "CDR" (lambda (c)
               (define p (cons 1 2))
               (set-cdr! p 7)
               (ok (cdr p))))
(define cow
  (evo:trial cand
             (hash "apply" (lambda (c)
                             (eq? (mutate:rebind "score"
                                                (evo:candidate-genome c)
                                                "child") #t))
                   "evaluate" (lambda (c)
                                (define v (vector 0))
                                (vector-set! v 0 (score 5))
                                (ok (vector-ref v 0))))))
(show "COW-STATUS" (hash-ref cow "status"))
(show "COW-ERR" (hash-ref cow "error" #f))
(show "COW-Y" (evo:objective cow "y"))
(show "COW-LIVE" (hash-ref cow "live-unchanged"))
(show "PARENT" (score 5))
(show "WS" (workspace :current))
(trial "CODE" (lambda (c)
                (define r (try (set-code "(define (score x) (* x 100))")
                               (catch (e) e)))
                (display "CODE ")
                (display r)
                (newline)
                (ok 0)))
(show "CODE-SCORE" (score 5))
(show "AFTER" (mutate:rebind "score" "(lambda (x) (* x 4))" "after"))
(eval-current)
(show "AFTER-SCORE" (score 5))
(show "AFTER-WS" (workspace :current))
"""


def env(log_path: Path) -> dict[str, str]:
    out = dict(os.environ)
    # The deny only shows while the sandbox stays Restricted.
    out.pop("AURA_SANDBOX", None)
    out.pop("AURA_PRESET", None)
    out.pop("AURA_PIPELINE_STRICT", None)
    out["AURA_EVO_LOG"] = str(log_path)
    return out


class TrialHeapUpdate(unittest.TestCase):
    def test_preset_dev_child_slot_updates(self) -> None:
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
                "PARENT-HASH 2",
                "PURE-STATUS ok",
                "PURE-ERR #f",
                "PURE-Y 1",
                "SET-STATUS ok",
                "SET-Y 4",
                "REF-STATUS ok",
                "REF-Y 3",
                "HSET-STATUS ok",
                "HSET-ERR #f",
                "HSET-Y 9",
                "VSET-STATUS ok",
                "VSET-ERR #f",
                "VSET-Y 8",
                "CAR-STATUS ok",
                "CAR-ERR #f",
                "CAR-Y 6",
                "HREM-STATUS ok",
                "HREM-ERR #f",
                "HREM-Y 0",
                "FILL-STATUS ok",
                "FILL-ERR #f",
                "FILL-Y 4",
                "CDR-STATUS ok",
                "CDR-ERR #f",
                "CDR-Y 7",
                "COW-STATUS ok",
                "COW-ERR #f",
                "COW-Y 45",
                "COW-LIVE #t",
                "PARENT 10",
                "WS 0",
                "CODE-STATUS ok",
                "CODE-ERR #f",
                "CODE-LIVE #t",
                "CODE-WS 0",
                "CODE-SCORE 10",
                "AFTER #t",
                "AFTER-SCORE 20",
                "AFTER-WS 0",
            ):
                self.assertIn(line, r.stdout, blob)
            self.assertIn("op=set-code", r.stdout)
            self.assertIn("effect-denied", r.stdout)


if __name__ == "__main__":
    unittest.main()
