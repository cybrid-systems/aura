#!/usr/bin/env python3
"""Regression for #4928: closures through map/apply return void after Moving.

Restricted session (`aura soft` / `--preset dev`) + AURA_PIPELINE_STRICT=
force-soa: after `set-code`, heap churn makes the primary arena auto-arm a
Moving compaction. The known-roots hook re-registered the Evaluator root
slots (workspace_flat_ / workspace_pool_ that cover the set-code
intermediates) only on ArenaGroup arenas, and the primary arena is not a
group member. The first Moving window consumed its slot list, so the next one
failed the #2971 pre-move gate, published a red densify window, and every
production apply_closure hard-refused: map consed (() ()) and apply gave ()
with no error. The hook now registers on the Evaluator's own arenas too, and a
densify refuse seen by a callback caller surfaces an error value instead of
void.

Second face (PROG_MUT): a `mutate:rebind` inside a RUNNING closure. The
Phase-5 Moving densify is soft-gated by the live temporary canaries of the
outer apply (nothing moves) and, per #4288, publishes a red window so the
mutate admit stays closed. The apply-side densify-stale refuse read that red
window as stale-reference evidence and refused every later closure apply
(`closure apply refused` / void). A window red only because the relocation
was refused before it moved anything now records itself as such
(`window_relocate_refused_clean`), and the apply arms fall through to the
LCP + remap tombstone checks.

The matrix covers {default, STRICT=0, force-soa} x {soft, --preset dev,
AURA_SANDBOX=off}, plus `aura repl` (which no longer turns Moving off).
The data generator is synthetic (deterministic LCG), no external data.
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
(define (rng-make seed) (list (modulo (+ (* seed 7919) 12345) 2147483647)))
(define (rng-next! r)
  (let ((v (modulo (+ (* (car r) 1103515245) 12345) 2147483648)))
    (set-car! r v)
    v))
(define (rng-float! r) (/ (* 1.0 (modulo (quotient (rng-next! r) 16) 1000000)) 1000000.0))
(define (churn seed n)
  (let* ((r (rng-make seed))
         (a (make-vector n 0.0))
         (b (make-vector (+ n 1) 0.0)))
    (let loop ((i 0) (p 100.0))
      (if (< i n)
          (let* ((x (- (let s ((k 0) (acc 0.0)) (if (= k 12) acc (s (+ k 1) (+ acc (rng-float! r))))) 6.0))
                 (p2 (* p (+ 1.0 (* 0.01 x)))))
            (vector-set! a i p2)
            (vector-set! b (+ i 1) (+ (vector-ref b i) p2))
            (loop (+ i 1) p2))
          #f))
    (hash "a" a "b" b "n" n)))
(set-code "(define (s) 1)")
(define (probe tag)
  (display (list tag (map (lambda (p) (+ p 1)) (list 1 2))
                 (apply (lambda (x y) (+ x y)) (list 1 2))))
  (newline))
(probe "p0")
(define d1 (churn 7 100))
(probe "p1")
(define d2 (churn 8 200))
(probe "p2")
(set-code "(define (s) 2)")
(define d3 (churn 9 200))
(probe "p3")
(define d4 (churn 10 400))
(probe "p4")
"""

EXPECTED = [f"(p{i} (2 3) 3)" for i in range(5)]

PROG_MUT = r"""
(set-code "(define (sp) (quote (1 2)))")
(eval-current)
(define (src-of i) (string-append "(lambda () (quote (" (string-join (map (lambda (x) (format "~a" x)) (list i (+ i 1))) " ") ")))"))
(define (step! i)
  (let* ((snap (mutate :snapshot (format "s-~a" i)))
         (ok (mutate :rebind "sp" (src-of i) "t")))
    (eval-current)
    (let ((r (list ok (map (lambda (x) (+ x 1)) (sp)) (string-join (map (lambda (x) (format "~a" x)) (sp)) " "))))
      (if (even? i) (begin (mutate :restore snap) (eval-current)) #f)
      r)))
(display (list "q" (map (lambda (i) (step! i)) (list 1 2 3 4 5 6))))
(newline)
"""

EXPECTED_MUT = "(q " + "(" + " ".join(f"(#t ({i + 1} {i + 2}) {i} {i + 1})" for i in range(1, 7)) + "))"
STRICTS = {"default": None, "strict0": "0", "force-soa": "force-soa"}
SESSIONS = {
    "soft": (["soft"], {}),
    "preset-dev": (["--preset", "dev"], {}),
    "sandbox-off": ([], {"AURA_SANDBOX": "off"}),
}


def _env(strict: str | None, extra: dict[str, str]) -> dict[str, str]:
    env = dict(os.environ)
    for k in ("AURA_PIPELINE_STRICT", "AURA_SANDBOX", "AURA_ARENA_MOVING_COMPACT", "AURA_KEEP_GOING"):
        env.pop(k, None)
    if strict is not None:
        env["AURA_PIPELINE_STRICT"] = strict
    env.update(extra)
    return env


class MovingClosureVoid(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not AURA_BIN.exists():
            raise unittest.SkipTest(f"aura binary not found: {AURA_BIN}")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        (cls.dir / "p4928.aura").write_text(PROG)
        (cls.dir / "m4928.aura").write_text(PROG_MUT)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_matrix(self) -> None:
        for sname, strict in STRICTS.items():
            for vname, (args, extra) in SESSIONS.items():
                with self.subTest(strict=sname, session=vname):
                    p = subprocess.run(
                        [str(AURA_BIN), *args, "p4928.aura"],
                        capture_output=True,
                        text=True,
                        env=_env(strict, extra),
                        cwd=self.dir,
                        timeout=180,
                        stdin=subprocess.DEVNULL,
                    )
                    self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                    lines = [ln for ln in p.stdout.splitlines() if ln.startswith("(p")]
                    self.assertEqual(lines, EXPECTED, p.stdout + p.stderr)

    def test_mutate_inside_running_closure(self) -> None:
        for sname, strict in STRICTS.items():
            for vname, (args, extra) in SESSIONS.items():
                with self.subTest(strict=sname, session=vname):
                    p = subprocess.run(
                        [str(AURA_BIN), *args, "m4928.aura"],
                        capture_output=True,
                        text=True,
                        env=_env(strict, extra),
                        cwd=self.dir,
                        timeout=180,
                        stdin=subprocess.DEVNULL,
                    )
                    self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                    self.assertNotIn("closure apply refused", p.stdout + p.stderr)
                    lines = [ln for ln in p.stdout.splitlines() if ln.startswith("(q")]
                    self.assertEqual(lines, [EXPECTED_MUT], p.stdout + p.stderr)

    def test_repl_force_soa(self) -> None:
        # `aura repl` used to default AURA_ARENA_MOVING_COMPACT=0 to hide this.
        for sname, strict in STRICTS.items():
            with self.subTest(strict=sname):
                p = subprocess.run(
                    [str(AURA_BIN), "repl", "p4928.aura"],
                    input='(probe "p5")\n',
                    capture_output=True,
                    text=True,
                    env=_env(strict, {}),
                    cwd=self.dir,
                    timeout=180,
                )
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                lines = [ln.strip() for ln in p.stdout.splitlines() if ln.strip().startswith("(p")]
                self.assertEqual(lines, EXPECTED + ["(p5 (2 3) 3)"], p.stdout + p.stderr)

    def test_repl_no_moving_off_default(self) -> None:
        src = (ROOT / "src" / "main.cpp").read_text()
        self.assertNotIn('setenv("AURA_ARENA_MOVING_COMPACT", "0"', src)


if __name__ == "__main__":
    unittest.main()
