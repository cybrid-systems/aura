#!/usr/bin/env python3
"""Concurrent (mutate :rebind ...) and set-code from fiber bodies: no deadlock, no crash.

Issue #5472: a body could reach mutate_mtx_ under the workspace lock (the IR
cache populate of a rebind add-path), the mirror image of the post-boundary
cascade drain, which takes mutate_mtx_ first and then waits for the workspace
lock in drop-jit-native. One fiber on each path deadlocked the process. The
mutation lock is taken before the workspace lock now (tests/compiler/
test_mutate_batch.cpp pins the ranks; this file runs the shape).

Issue #5473: the evolution-log step around every mutate:* read the workspace
without the shared lock and raced a peer's rebind (FlatAST precondition
abort, SIGSEGV in the tag/arity index).

Issue #5475: the tail of set-code (the define re-bind, the epoch stamps) ran
with no lock after its boundary; a peer fiber's rebind on the same FlatAST /
StringPool raced it (an intern spinning forever with the pool lock held, a
rebind dying on a torn FlatAST).

Issue #5477: a body that called a workspace define while a sibling rebind +
eval-current replaced that define's body could read the recycled node slot
after the AST was rebuilt. The child ids in the held NodeView could then
name another form, and the caller ran it in its own environment (about 2-4%
of runs ended in "expected Int, got Closure" inside the wrong define).

The follow-up keeps the caller running instead of failing closed: an applied
or inlined workspace body registers as a flat reader for its whole frame, so
a concurrent rebind appends new nodes instead of reusing/recycling the old
body's slots; the child/param id list is snapshotted before dispatch. A
rebind that lands between the callee lookup and the inline is retried from
the live cell. The program must finish successfully and the caller must
never observe a wrong node.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))
from _aura_harness import AURA_BIN  # noqa: E402

CRASH_MARKERS = (
    "AURA CRASH",
    "contract violation",
    "double free",
    "corrupted",
    "Segmentation fault",
    "Resource deadlock avoided",
)

WORKSPACE = '"(define (pol-0 x) (+ x 0))\\n(define (pol-1 x) (+ x 1))"'

# Issue #5472. `adder` rebinds names that do not exist (the add path and its
# IR cache populate), `rebinder` rebinds an existing define and re-evaluates
# (the cascade drain), each in its own fiber.
LOCK_ORDER_PROGRAM = textwrap.dedent(
    f"""\
    (set-code {WORKSPACE})
    (eval-current)
    (define (adder n)
      (let loop ((i 0))
        (if (< i n)
            (begin
              (mutate :rebind (string-append "fresh-" (number->string i))
                      "(lambda (x) (+ x 1))" "add")
              (loop (+ i 1)))
            'adder)))
    (define (rebinder n)
      (let loop ((i 0))
        (if (< i n)
            (begin
              (mutate :rebind "pol-0"
                      (string-append "(lambda (x) (+ x " (number->string i) "))") "p")
              (eval-current)
              (loop (+ i 1)))
            'rebinder)))
    (display (map fiber:join
                  (list (fiber:spawn (lambda () (adder 120)))
                        (fiber:spawn (lambda () (rebinder 120))))))
    (newline)
    """
)

# Issue #5475. `reloader` installs the same workspace again and again; each
# install ends with a define re-bind that used to run after its boundary.
RELOAD_PROGRAM = textwrap.dedent(
    f"""\
    (set-code {WORKSPACE})
    (eval-current)
    (define src {WORKSPACE})
    (define (reloader n)
      (let loop ((i 0))
        (if (< i n)
            (begin
              (set-code src)
              (eval-current)
              (loop (+ i 1)))
            'reloader)))
    (define (rebinder n)
      (let loop ((i 0))
        (if (< i n)
            (begin
              (mutate :rebind "pol-0"
                      (string-append "(lambda (x) (+ x " (number->string i) "))") "p")
              (eval-current)
              (loop (+ i 1)))
            'rebinder)))
    (display (map fiber:join
                  (list (fiber:spawn (lambda () (reloader 300)))
                        (fiber:spawn (lambda () (rebinder 300))))))
    (newline)
    """
)

# Issue #5477. One fiber calls a workspace define in a tight loop; the other
# rebinds that define and re-evaluates the workspace, recycling the node slots
# the caller is walking. JIT stays at its default here: the failing runs only
# appear on that face.
RACE_PROGRAM = textwrap.dedent(
    f"""\
    (set-code {WORKSPACE})
    (eval-current)
    (define (rebinder n)
      (let loop ((i 0))
        (if (< i n)
            (begin
              (mutate :rebind \"pol-0\"
                      (string-append \"(lambda (x) (+ x \" (number->string i) \"))\") \"p\")
              (eval-current)
              (loop (+ i 1)))
            'rebinder)))
    (define (caller n)
      (let loop ((i 0) (bad 0))
        (if (< i n)
            (let ((r (let ((x -100000)) (pol-0 1000))))
              (if (and (integer? r) (>= r 1000) (< r (+ 1000 n)))
                  (loop (+ i 1) bad)
                  (loop (+ i 1) (+ bad 1))))
            bad)))
    (display (map fiber:join
                  (list (fiber:spawn (lambda () (rebinder 2000)))
                        (fiber:spawn (lambda () (caller 20000))))))
    (newline)
    """
)

# The shape of test_fiber_hash_key_survives_rebind_4709: four bodies, each
# re-binding its own policy and re-evaluating while the others do the same.
STORM_PROGRAM = textwrap.dedent(
    """\
    (set-code "(define (pol-0 x) (+ x 0))\\n(define (pol-1 x) (+ x 1))\\n(define (pol-2 x) (+ x 2))\\n(define (pol-3 x) (+ x 3))")
    (eval-current)
    (define stats (make-hash))
    (hash-set! stats "join-void" 0)
    (define (worker w)
      (let loop ((i 0))
        (if (< i 30)
            (begin
              (hash-set! stats (string-append "join-void") (+ 1 (hash-ref stats "join-void" 0)))
              (mutate :rebind (string-append "pol-" (number->string w)) "(lambda (x) (+ x 1))" "p")
              (eval-current)
              (loop (+ i 1)))
            w)))
    (define fs (list (fiber:spawn (lambda () (worker 0)))
                     (fiber:spawn (lambda () (worker 1)))
                     (fiber:spawn (lambda () (worker 2)))
                     (fiber:spawn (lambda () (worker 3)))))
    (map fiber:join fs)
    (list (map string-length (hash-keys stats)) (hash-ref stats "join-void" -1))
    """
)


def _two_cpus_if_possible() -> list[str] | None:
    """`taskset -c a,b` when available: the races need the bodies to preempt each other."""
    if os.environ.get("AURA_TEST_NO_AFFINITY"):
        return None
    cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    if len(cpus) < 2:
        return None
    return ["taskset", "-c", ",".join(str(c) for c in cpus[:2])]


class ConcurrentRebind(unittest.TestCase):
    def run_program(self, program: str, timeout: float, jit: bool = False) -> subprocess.CompletedProcess[str]:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = {
            **os.environ,
            "AURA_PIPELINE_STRICT": "0",
            "AURA_SANDBOX": "off",
        }
        if not jit:
            env["AURA_TIER_JIT"] = "0"
        with tempfile.NamedTemporaryFile("w", suffix=".aura", prefix="aura-rebind-", delete=False) as fh:
            fh.write(program)
            path = fh.name
        try:
            prefix = _two_cpus_if_possible() or []
            run = subprocess.run(
                [*prefix, str(AURA_BIN), path],
                capture_output=True,
                text=True,
                errors="replace",
                timeout=timeout,
                env=env,
                cwd=str(ROOT),
            )
        except subprocess.TimeoutExpired:
            self.fail(f"the program did not finish in {timeout:g}s (deadlock)")
        finally:
            os.unlink(path)
        for marker in CRASH_MARKERS:
            self.assertNotIn(marker, run.stderr, f"crash marker {marker!r} in stderr:\n{run.stderr[-600:]}")
        self.assertNotIn("unbound variable", run.stderr, run.stderr[-600:])
        return run

    def test_add_path_rebind_next_to_a_cascade_drain(self) -> None:
        # Issue #5472: both fibers finish and the program prints their results.
        for _ in range(5):
            run = self.run_program(LOCK_ORDER_PROGRAM, timeout=90)
            self.assertEqual(run.returncode, 0, run.stderr[-600:])
            lines = [ln.strip() for ln in run.stdout.splitlines() if ln.strip()]
            self.assertEqual(lines[-1:] or [None], ["(adder rebinder)"], run.stdout[-300:])

    def test_set_code_reload_next_to_a_rebind(self) -> None:
        # Issue #5475: no hang, no crash; both fibers finish.
        for _ in range(40):
            run = self.run_program(RELOAD_PROGRAM, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr[-600:])
            lines = [ln.strip() for ln in run.stdout.splitlines() if ln.strip()]
            self.assertEqual(lines[-1:] or [None], ["(reloader rebinder)"], run.stdout[-300:])

    def test_define_call_next_to_rebind_eval_current_jit(self) -> None:
        # Issue #5477 follow-up: the caller now finishes successfully while
        # the sibling keeps rebinding. It must never run the wrong node.
        for _ in range(15):
            run = self.run_program(RACE_PROGRAM, timeout=45, jit=True)
            out = (run.stdout or "") + (run.stderr or "")
            self.assertEqual(run.returncode, 0, run.stderr[-600:])
            self.assertNotIn("expected Int, got Closure", out, out[-600:])
            self.assertNotIn("invalid closure: eval_flat", out, out[-600:])
            self.assertNotIn("stale workspace", out, out[-600:])
            lines = [ln.strip() for ln in run.stdout.splitlines() if ln.strip()]
            self.assertEqual(lines[-1:] or [None], ["(rebinder 0)"], run.stdout[-300:])

    def test_rebind_and_eval_current_storm(self) -> None:
        # Issues #5472 / #5473: no hang, no crash, the key and its value survive.
        for _ in range(40):
            run = self.run_program(STORM_PROGRAM, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr[-600:])
            lines = [ln.strip() for ln in run.stdout.splitlines() if ln.strip()]
            last = lines[-1] if lines else ""
            self.assertTrue(last.startswith("((9) ") and last.endswith(")"), f"got {last!r}")


if __name__ == "__main__":
    unittest.main()
