#!/usr/bin/env python3
"""Issue #2727: per-Fiber durable evaluator_id (#2721 residual — steal_safety
hard-AND predicate (d) GC-defer arm state addressed the victim's evaluator
via mutation_stack_ptr() proxy; #2727 adds a first-class Fiber::evaluator_id_
field set on MutationBoundaryGuard enter / cleared on exit).

Contract (one row per AC):
  AC1 src/serve/fiber.h defines std::atomic<void*> evaluator_id_ (private)
     + public evaluator_id() reader (atomic load) +
     set_evaluator_id(void*) / clear_evaluator_id() setters (atomic store).
     src/serve/fiber.cpp aura_fiber_evaluator_id_for_steal_safety strong
     def returns the stored evaluator_id() (not mutation_stack_ptr()).
  AC2 src/compiler/evaluator_mutation_boundary.cpp Guard ctor sets
     evaluator_id_ on outermost enter (this Evaluator); Guard dtor
     clears it at end (atomic store nullptr). Nested guards inherit
     the outermost's id (no set/clear in nested path).
  AC3 C-linkage signature unchanged (extern "C" void* ...) so Soft +
     production paths are identical and existing #2721 hard-AND
     counters continue to attribute correctly. The proxy swap is
     transparent to steal_safety.cpp callers.
  AC4 src/serve/steal_safety.cpp calls aura_fiber_evaluator_id_for_steal_safety
     for the victim (used by gc_deferred_for_evaluator in predicate (d)
     — GC-defer arm state). Replaces the prior mutation_stack_ptr()
     proxy with the durable per-Fiber evaluator_id.
  AC5 Source-cite + linter: tests/serve/test_steal_complete_restamp_txn.cpp
     extended with ac2727_1..5 per #81967 (no new test file). build.py
     wires the linter. No docs/design/2727-* on disk per #1655.
  AC6 Performance: zero cost on hot steal path (one extra atomic load
     — same cost as the prior mutation_stack_ptr() proxy it replaces;
     accessors are atomic load / store release / store release).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    fbc = _read("src/compiler/fiber_bridge.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    sc = _read("src/serve/steal_safety.cpp")
    t = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    # AC1 — Fiber stores durable evaluator_id + accessors; strong def returns it.
    must("evaluator_id_", "AC1", fh)
    must("evaluator_id()", "AC1", fh)
    must("set_evaluator_id", "AC1", fh)
    must("clear_evaluator_id", "AC1", fh)
    must("Issue #2727", "AC1", fh)
    must("return fb->evaluator_id()", "AC1", fc)
    must("Issue #2727", "AC1", fc)

    # AC2 — Guard ctor sets + dtor clears (outermost only).
    must("set_evaluator_id(static_cast<void*>(ev_))", "AC2", emb)
    must("clear_evaluator_id()", "AC2", emb)
    must("is_outermost_ && aura::serve::g_current_fiber", "AC2", emb)
    must("Issue #2727", "AC2", emb)

    # AC3 — Soft + production paths identical; C-linkage signature preserved.
    must('extern "C" void* aura_fiber_evaluator_id_for_steal_safety', "AC3", fc)
    must("aura_fiber_evaluator_id_for_steal_safety", "AC3", fbc)  # weak stub preserved

    # AC4 — steal_safety.cpp calls the new getter for GC-defer residual.
    must("aura_fiber_evaluator_id_for_steal_safety", "AC4", sc)
    must("victim_eval_id", "AC4", sc)
    # The prior #2721 follow-up note in fiber.cpp explicitly mentions
    # #2727 as the fix (source-cite the migration).
    must("#2727", "AC4", fc)
    must("#2721", "AC4", sc)  # lineage

    # AC5 — source-cite + test extension per #81967 + build.py wires + no docs.
    must("ac2727_1_evaluator_id_set_on_guard_enter", "AC5", t)
    must("ac2727_2_evaluator_id_cleared_on_guard_exit", "AC5", t)
    must("ac2727_3_soft_and_production_identical", "AC5", t)
    must("ac2727_4_gc_defer_residual_under_known_evaluator", "AC5", t)
    must("ac2727_5_source_and_linter", "AC5", t)
    # #81967: NO new test file — extend the existing one.
    if (ROOT / "tests" / "serve" / "test_issue_2727.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_2727.cpp present (forbidden per #81967)")
    must("check_fiber_evaluator_id_2727", "AC5", build)
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2727-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    # AC6 — zero-cost hot path: atomic load (release on set / acquire on read).
    must("memory_order_release", "AC6", fh)
    must("memory_order_acquire", "AC6", fh)
    # The strong def returns the stored id (atomic load — same cost as the
    # prior mutation_stack_ptr() proxy).
    must("return fb->evaluator_id()", "AC6", fc)

    # Cross-check: prior #2726 + #2721 + #2699 linters still green (the
    # additive superset path). #2727 sits on top of #2721 steal_safety
    # + #2720/#2724 surfaces.
    for prev in (
        "check_cross_fiber_hold_budget_cancel_2726.py",
        "check_steal_safety_transaction_2699.py",
        "check_handoff_ref_mailbox_gate_2700.py",
    ):
        prev_path = ROOT / "scripts" / "coverage" / "checks" / prev
        if not prev_path.is_file():
            continue
        r = subprocess.run(
            [sys.executable, str(prev_path)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{prev} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2727 fiber evaluator_id — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
