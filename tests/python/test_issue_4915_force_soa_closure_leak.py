#!/usr/bin/env python3
"""Regression for #4915: force-soa self-evolution loop leaks closures and slows linearly.

Under AURA_PIPELINE_STRICT=force-soa a top-level `(define out (evolve! ...))`
runs the whole loop nested under a suspended IR drain. When the loop body lives
in a `require`d module, the tree-walker sweep (reclaim_tw_unreachable_now)
stood down because *any* IR drain was active (#4747), so every closure the
loop allocated survived: ~2000 closures/step here, ~8000/step in the
evolve-strategy starter kit, and mutate:rebind (O(live closures)) went from
1 ms to 2 s over 20 steps. The sweep now stands down only for IR drains on
other threads / fibers and roots the same-thread drains' closure envs.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

STEPS = 16

EV = """
(define (work n k)
  (let loop ((i 0) (acc 0))
    (if (< i n)
        (loop (+ i 1) (+ acc (apply + (map (lambda (x) (+ x i k)) (list 1 2 3)))))
        acc)))
(define (evolve! steps)
  (let loop ((s 1) (out (list)))
    (if (> s steps)
        (reverse out)
        (let* ((snap (mutate :snapshot (format "s-~a" s)))
               (r (mutate:rebind "strategy-params"
                                 (format "(lambda () (quote ((fast . ~a))))" s) "t")))
          (eval-current)
          (let ((w (work 2000 (cdr (car (strategy-params))))))
            (if (even? s) (begin (mutate :restore snap) (eval-current)) #f)
            (loop (+ s 1) (cons (list s r w (stats:get "gc-stats")) out)))))))
"""

MAIN = """
(set-code "(define strategy-params (lambda () (quote ((fast . 2)))))")
(eval-current)
(require "./ev")
(define out (evolve! STEPS))
(for-each (lambda (row) (display row) (newline)) out)
(display (list "final" (strategy-params))) (newline)
""".replace("STEPS", str(STEPS))


class ForceSoaClosureLeak(unittest.TestCase):
    def run_mode(self, pipeline: str | None) -> list[tuple[int, int]]:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "ev.aura").write_text(EV)
            (Path(d) / "m.aura").write_text(MAIN)
            env = dict(os.environ)
            env["AURA_SANDBOX"] = "off"
            env.pop("AURA_PIPELINE_STRICT", None)
            if pipeline:
                env["AURA_PIPELINE_STRICT"] = pipeline
            r = subprocess.run([str(AURA_BIN), "m.aura"], cwd=d, capture_output=True, text=True, env=env, timeout=300)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        rows = []
        for line in r.stdout.splitlines():
            m = re.match(r"\((\d+) #t (\d+) .*cls:(\d+)", line)
            if m:
                rows.append((int(m.group(1)), int(m.group(3))))
        self.assertEqual([s for s, _ in rows], list(range(1, STEPS + 1)), r.stdout[-2000:])
        # odd steps are kept, even steps restored: last kept value is STEPS-1
        self.assertIn(f"(final ((fast . {STEPS - 1})))", r.stdout)
        return rows

    def assert_bounded(self, rows: list[tuple[int, int]]) -> None:
        cls = [c for _, c in rows]
        # before the fix: +2000 closures per step (≈32000 at step 16)
        self.assertLess(max(cls) - min(cls), 1000, cls)
        self.assertLess(cls[-1], 2000, cls)

    def test_force_soa_bounded(self) -> None:
        self.assert_bounded(self.run_mode("force-soa"))

    def test_default_bounded(self) -> None:
        self.assert_bounded(self.run_mode(None))


if __name__ == "__main__":
    unittest.main()
