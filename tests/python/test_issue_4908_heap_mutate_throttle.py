#!/usr/bin/env python3
"""Regression for #4908: an ordinary set-car! is not refused as "naked mutate".

Before: in the Restricted default with `--preset dev` and
AURA_PIPELINE_STRICT=force-soa, a program that loaded code with set-code and
then filled two synthetic series with set-car! / vector-set! failed on the
second series with "naked mutate: set-car! requires MutationBoundaryGuard":
the moving-densify agent throttle (meant for AST mutation) refused the
heap-slot guard. sandbox=off and the default pipeline ran the same program.
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
(define (rng-make seed) (list (modulo (+ (* seed 7919) 12345) 2147483647)))
(define (rng-next! r)
  (let ((v (modulo (+ (* (car r) 1103515245) 12345) 2147483648)))
    (set-car! r v)
    v))
(define (rng-float! r) (/ (* 1.0 (modulo (quotient (rng-next! r) 16) 1000000)) 1000000.0))
(define (rng-normal! r)
  (let loop ((i 0) (s 0.0))
    (if (= i 12) (- s 6.0) (loop (+ i 1) (+ s (rng-float! r))))))
(define (series seed n)
  (let ((r (rng-make seed)) (v (make-vector n 0.0)))
    (let loop ((i 0))
      (if (< i n) (begin (vector-set! v i (rng-normal! r)) (loop (+ i 1))) v))))
(set-code "(define (strategy-params) (quote ((fast . 2))))")
(define a (series 7 1000))
(define b (series 11 1000))
(display (list "n" (vector-length a) (vector-length b))) (newline)
"""


class HeapMutateThrottle(unittest.TestCase):
    def test_force_soa_restricted_matches_off(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(PROG)
            for extra, args in (
                ({"AURA_SANDBOX": "off"}, ()),
                ({}, ("--preset", "dev")),
                ({"AURA_PIPELINE_STRICT": "force-soa"}, ("--preset", "dev")),
            ):
                with self.subTest(extra=extra):
                    env = dict(os.environ)
                    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT"):
                        env.pop(k, None)
                    env.update(extra)
                    r = subprocess.run(
                        [str(AURA_BIN), *args, "p.aura"],
                        cwd=d,
                        capture_output=True,
                        text=True,
                        env=env,
                        timeout=300,
                    )
                    self.assertEqual(r.returncode, 0, r.stderr)
                    self.assertIn("(n 1000 1000)", r.stdout)
                    self.assertNotIn("naked mutate", r.stderr)
                    self.assertNotIn("mutation refused", r.stderr)  # #4909 label


if __name__ == "__main__":
    unittest.main()
