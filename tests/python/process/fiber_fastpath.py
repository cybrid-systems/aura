"""Issue #5066: the CLI worker cap must not change the fiber checksum.

--worker-threads 1 runs the bodies one at a time. --worker-threads 4
runs them together. Both match the single-threaded sum. The fast-path
and checksum assertions themselves live in the suite files.
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

CHECK = ROOT / "tests" / "suite" / "issue_5066_fiber_checksum.aura"


def _run(args: list[str]) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    return subprocess.run(
        [str(AURA_BIN), *args],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )


class FiberFastPath5066(unittest.TestCase):
    def test_worker_cap_keeps_checksum(self) -> None:
        for workers in ("1", "4"):
            r = _run(["--worker-threads", workers, "--load", str(CHECK)])
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("0 failed", r.stdout)

    def test_float_and_dead_pair_named_let(self) -> None:
        # Tree walker, not the JIT float shadow. Exact results only.
        src = """
(define (w-int k)
  (let loop ((i 0) (s 0))
    (if (< i 2000) (loop (+ i 1) (+ s 3)) s)))
(define (w-float k)
  (let loop ((i 0) (s 0.0))
    (if (< i 2000) (loop (+ i 1) (+ s 1.5)) s)))
(define (w-alloc k)
  (let loop ((i 0) (s 0))
    (if (< i 2000) (loop (+ i 1) (+ s (car (list 3 i)))) s)))
(define (w-cons k)
  (let loop ((i 0) (s 0))
    (if (< i 2000) (loop (+ i 1) (+ s (car (cons 3 i)))) s)))
(define (w-cmp k)
  (let loop ((i 0) (s 0.0))
    (if (< s 10.0) (loop (+ i 1) (+ s 1.5)) s)))
(define (w-ge k)
  (let loop ((i 0) (s 0.0))
    (if (>= i 4) s (loop (+ i 1) (+ s 1.5)))))
(define (w-nest k)
  (let outer ((n 0) (acc 0.0))
    (if (< n 20)
        (outer (+ n 1)
               (let inner ((i 0) (s acc))
                 (if (< i 10) (inner (+ i 1) (+ s 1.5)) s)))
        acc)))
(define (w-sub k)
  (let loop ((i 0) (s 0.0))
    (if (< i 3) (loop (+ i 1) (- s 1.5)) s)))
(define (w-head k)
  (let loop ((i 0) (s 0))
    (if (< i 4) (loop (+ i 1) (+ s (car (cons (+ i 2) i)))) s)))
(define (w-escape k)
  (let lp ((acc 0.0) (i 0) (snap '()))
    (if (< i 5)
        (lp (+ acc 1.0) (+ i 1) (cons acc snap))
        (let sum ((l snap) (t 0.0))
          (if (null? l) t (sum (cdr l) (+ t (car l))))))))
(define (w-shadow k)
  (let ((car (lambda (x) 7)))
    (let loop ((i 0) (s 0))
      (if (< i 3) (loop (+ i 1) (+ s (car (list 3 i)))) s))))
(define (show x) (display x) (newline))
(show (w-int 0))
(show (w-float 0))
(show (w-alloc 0))
(show (w-cons 0))
(show (w-cmp 0))
(show (w-ge 0))
(show (w-nest 0))
(show (let ((n 1.5))
        (let loop ((i 0) (s 0.0))
          (if (< i 4) (loop (+ i 1) (+ s n)) s))))
(show (w-sub 0))
(show (w-head 0))
(show (w-escape 0))
(show (w-shadow 0))
(define (par f)
  (list (fiber:join (fiber:spawn (lambda () (f 0))))
        (fiber:join (fiber:spawn (lambda () (f 0))))))
(show (par w-float))
(show (par w-alloc))
(show (par w-cons))
"""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p.aura"
            path.write_text(src)
            r = _run(["--quiet", "--preset", "dev", str(path)])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(
            r.stdout.splitlines(),
            [
                "6000",
                "3000.0",
                "6000",
                "6000",
                "10.5",
                "6.0",
                "300.0",
                "6.0",
                "-4.5",
                "14",
                "10.0",
                "21",
                "(3000.0 3000.0)",
                "(6000 6000)",
                "(6000 6000)",
            ],
        )
        over = """
(define (over)
  (let loop ((i 0) (s 4611686018427387900))
    (if (< i 10) (loop (+ i 1) (+ s 1)) s)))
(over)
"""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "over.aura"
            path.write_text(over)
            bad = _run(["--quiet", "--preset", "dev", str(path)])
        self.assertNotEqual(bad.returncode, 0, bad.stdout + bad.stderr)
        self.assertIn("integer overflow", bad.stdout + bad.stderr)


if __name__ == "__main__":
    unittest.main()
