#!/usr/bin/env python3
"""Regression test for issue #4859 — (eval ...) program retention.

#4359 kept the parsed FlatAST/StringPool of an (eval ...) program for the
whole Evaluator lifetime, because a Closure built by that program points
at Closure::flat / Closure::pool (raw pointers).

#4854 made that retention CONDITIONAL — the program was kept only when the
parsed tree contained a Lambda node — which fixed the (eval datum) growth
but left the closure-producing case growing ~12 KB per eval in a
long-running host until the Evaluator died.

#4859 replaces the Evaluator-lifetime retention with LIFETIME OWNERSHIP:
the parsed program is owned by the holders that name it (the closures and
the macros built from it, via a shared_ptr<EvalProgram>), so it is
reclaimed as soon as the LAST holder is gone.

Acceptance covered here:

  1. (eval '(lambda (x) (+ x 1))) in a loop keeps VmRSS FLAT after
     (gc-heap): the closure is dropped, so the program goes with it.
  2. A closure created by eval KEEPS WORKING after the eval primitive
     returns — a plain call (apply_closure), a call as a map callback
     (the closure bridge path) and a call from the IR-compiled top-level
     program (--load lowers the program to IR).
  3. The eval'd closure still works after a (gc-heap) that collected the
     other closures, and an eval'd macro still expands (a macro is a
     global registry entry and keeps its program alive).
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

BATCHES = 6
WARMUP = 50

# The VmRSS probe from #4854 (tests/python/process/test_orch_rss.py) — the
# repo's established RSS shape.
_RSS = r"""

(define (rss-kb)
  (let ((s (read-file "/proc/self/status")))
    (let ((n (string-length s)) (i 0) (found -1))
      (while (lambda () (and (= found -1) (< i (- n 6))))
        (lambda ()
          (if (equal? (substring s i (+ i 6)) "VmRSS:") (set! found i) 0)
          (set! i (+ i 1))))
      (if (= found -1) 0
          (let ((k (+ found 6)) (v 0) (done #f))
            (while (lambda () (and (< k n) (or (= (string-ref s k) 32) (= (string-ref s k) 9))))
              (lambda () (set! k (+ k 1))))
            (while (lambda () (and (not done) (< k n)))
              (lambda ()
                (let ((c (string-ref s k)))
                  (if (and (>= c 48) (<= c 57))
                      (begin (set! v (+ (* v 10) (- c 48))) (set! k (+ k 1)))
                      (set! done #t)))))
            v)))))

(define *b* 0)
(define (work-batch k)
  (let ((i 0))
    (while (lambda () (< i k))
      (lambda ()
        __BODY__
        (set! i (+ i 1))))))

(work-batch __WARMUP__)
(gc-heap)
(define *base* (rss-kb))
(display "START ") (display *base*) (newline)
(define *prev* *base*)
(while (lambda () (< *b* __BATCHES__))
  (lambda ()
    (work-batch __SIZE__)
    (set! *b* (+ *b* 1))
    (gc-heap)
    (let ((now (rss-kb)))
      (display "BATCH ") (display *b*) (display " ") (display now)
      (display " DELTA ") (display (- now *prev*)) (newline)
      (set! *prev* now))))
"""

# The closure-producing eval. The lambda is DROPPED (the eval's value is
# discarded), so after (gc-heap) nothing holds the closure — and with
# #4859 nothing holds the parsed program either.
_LAMBDA_EVAL_BODY = """(let ((c (eval '(lambda (x) (+ x 1))))) 0)"""

# A bigger eval'd program: the pre-fix retention is the WHOLE parsed
# program per eval, so a bigger program makes the linear growth obvious
# (measured pre-fix: ~19 KB per eval on this tree).
_NUMBERS = " ".join(str(i) for i in range(1, 31))
_BIG_LAMBDA_EVAL_BODY = "(let ((c (eval '(lambda (x) (+ x " + _NUMBERS + "))))) 0)"

# The RSS budget per probe: the post-warmup growth budget in KB.
#
# Measured on the pre-fix tree (0a2ab76b0) and on the fixed tree, 6
# batches of 500 evals, post-warmup batches only:
#
#   probe            pre-fix sum / max      post-fix sum / max
#   small lambda     10256 KB / 2064 KB      16..2064 KB / <= 340 KB
#   big lambda       47828 KB / 10672 KB     3800..5528 KB / <= 2464 KB
#
# The pre-fix numbers are LINEAR (every batch grows by the same amount —
# the parsed program is retained for the Evaluator lifetime). The post-fix
# numbers are the allocator high-water steps of the per-eval
# allocate/free churn (they also show up when the eval'd program builds
# NO closure at all), so they are bounded and bursty.
_SMALL_LAMBDA_BUDGET_KB = 4096
_BIG_LAMBDA_BUDGET_KB = 12288
_BIG_LAMBDA_BATCH_BUDGET_KB = 6144


def _render(body: str, size: int) -> str:
    return (
        _RSS.replace("__BODY__", body)
        .replace("__SIZE__", str(size))
        .replace("__WARMUP__", str(WARMUP))
        .replace("__BATCHES__", str(BATCHES))
    )


def _run(source: str, timeout: float = 240.0) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as f:
        f.write(source)
        path = f.name
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    # The strict pipeline gate refuses this path with an internal error
    # instead of the real symptom; the permissive walker face is the one
    # the script face uses (#4848).
    env["AURA_PIPELINE_STRICT"] = "0"
    try:
        return subprocess.run(
            [str(AURA_BIN), "--load", path],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
            stdin=subprocess.DEVNULL,
        )
    finally:
        os.unlink(path)


class TestIssue4859EvalProgramOwnership(unittest.TestCase):
    def _probe_deltas(self, body: str, size: int = 500) -> tuple[int, list[int]]:
        """Run the RSS probe and return (start KB, per-batch deltas KB)."""
        proc = _run(_render(body, size))
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        start = None
        deltas: list[int] = []
        for line in proc.stdout.splitlines():
            m = re.match(r"START (\d+)$", line)
            if m:
                start = int(m.group(1))
                continue
            m = re.match(r"BATCH \d+ (\d+) DELTA (-?\d+)$", line)
            if m:
                deltas.append(int(m.group(2)))
        self.assertIsNotNone(start, f"no START line in probe output:\n{proc.stdout[-3000:]}")
        self.assertEqual(len(deltas), BATCHES, f"expected {BATCHES} samples:\n{proc.stdout[-3000:]}")
        assert start is not None
        return start, deltas

    def test_4859_lambda_eval_rss_is_flat(self) -> None:
        """(eval '(lambda ...)) must not grow VmRSS once the closure is gone."""
        start, deltas = self._probe_deltas(_LAMBDA_EVAL_BODY)
        # Pre-fix: every post-warmup batch grew by the same amount (the
        # parsed program stayed alive for the Evaluator lifetime) —
        # 10256 KB over the five post-warmup batches. Post-fix the growth
        # is the allocator high-water of the per-eval allocate/free churn
        # (16..2064 KB), so the budget sits ~2x above the observed noise
        # and ~2.5x below the old linear rate.
        growth = sum(deltas[1:])
        self.assertLess(
            growth,
            _SMALL_LAMBDA_BUDGET_KB,
            f"#4859: VmRSS grew {growth} KB over {(BATCHES - 1) * 500} "
            f"(eval '(lambda ...)) calls (limit {_SMALL_LAMBDA_BUDGET_KB} KB, "
            f"start {start} KB, deltas {deltas}). The parsed program is "
            f"still retained.",
        )

    def test_4859_big_lambda_eval_rss_is_flat(self) -> None:
        """A bigger eval'd program must not be retained per eval either."""
        start, deltas = self._probe_deltas(_BIG_LAMBDA_EVAL_BODY)
        growth = sum(deltas[1:])
        worst = max(deltas[1:])
        self.assertLess(
            growth,
            _BIG_LAMBDA_BUDGET_KB,
            f"#4859: VmRSS grew {growth} KB over {(BATCHES - 1) * 500} "
            f"(eval '(lambda ...)) calls (limit {_BIG_LAMBDA_BUDGET_KB} KB, "
            f"start {start} KB, deltas {deltas}). The parsed program is "
            f"still retained.",
        )
        # The pre-fix shape was LINEAR: every batch grew by ~9-10 MB. A
        # single batch must not grow by more than the budget either.
        self.assertLess(
            worst,
            _BIG_LAMBDA_BATCH_BUDGET_KB,
            f"#4859: one batch grew {worst} KB (limit "
            f"{_BIG_LAMBDA_BATCH_BUDGET_KB} KB, start {start} KB, deltas "
            f"{deltas}) — the growth looks linear.",
        )

    def test_4859_eval_closure_still_works(self) -> None:
        """A closure built by eval keeps working after eval returns."""
        proc = _run(
            """
(define f (eval '(lambda (x) (+ x 1))))
(display "direct ") (display (f 41)) (newline)
(display "mapped ") (display (map f (list 1 2 3))) (newline)
(define g (eval '(lambda (y) (lambda (x) (+ x y)))))
(display "nested ") (display ((g 7) 3)) (newline)
(display "capture ")
(display ((eval '(let ((y 10)) (lambda (x) (+ x y)))) 5)) (newline)
""",
            timeout=60.0,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        out = proc.stdout
        self.assertIn("direct 42", out)
        self.assertIn("mapped (2 3 4)", out)
        self.assertIn("nested 10", out)
        self.assertIn("capture 15", out)

    def test_4859_eval_closure_survives_gc(self) -> None:
        """The program stays alive while its closure does — even after a gc."""
        proc = _run(
            """
(define f (eval '(lambda (x) (+ x 1))))
(gc-heap)
(display "after-gc ") (display (f 41)) (newline)
(gc-heap)
(display "after-gc2 ") (display (f 1)) (newline)
""",
            timeout=60.0,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        self.assertIn("after-gc 42", proc.stdout)
        self.assertIn("after-gc2 2", proc.stdout)

    def test_4859_eval_macro_still_expands(self) -> None:
        """A macro registered by eval keeps its program alive and expands."""
        proc = _run(
            """
(eval '(defmacro (twice x) (list (quote +) x x)))
(display "macro ") (display (twice 21)) (newline)
""",
            timeout=60.0,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        self.assertIn("macro 42", proc.stdout)


if __name__ == "__main__":
    unittest.main()
