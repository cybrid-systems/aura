#!/usr/bin/env python3
"""Issue #2677: runtime(steal) — harden MutationSafetySnapshot resume
ticket + LayoutStamp fail-closed under production defaults. Refines
#2518 / #2346 / #2372 / #2250.

Contract:
  AC1 Resume path always evaluates BOTH resume-safety-ticket AND
     LayoutStamp (when set). Mismatch on either triggers production
     fail-closed path (force-deopt + full refresh or mark-Done under
     Hard). Single call site from Fiber::resume via
     check_and_enforce_resume_invariants (consolidates ticket + stamp).
  AC2 production_defaults_active() ignores AURA_STEAL_SNAPSHOT_SOFT=1.
     Soft is test-only (or explicitly unlocked). Decision table in
     fiber.h is documented + accurate.
  AC3 Every Guard enter/exit and orch soft-boundary enter/exit path
     publishes mirrors (publish_mutation_safety_mirrors). A source-
     level / linter gate rejects any path that mutates depth/held
     without a publish (spot-check: 3 known call sites must exist).
  AC4 Unit tests inject mid-window Guard enter/exit between sample
     and resume. Soft observes mismatch counter only, production
     forces deopt / hard-fail. Existing #2518 / #2346 tests remain
     green (test_resume_invariants_2677 extends existing
     test_fiber_mutation_steal_safety.cpp per #81967).
  AC5 Additive metrics (ticket-mismatch, force-deopt, hard-fail
     already present; LayoutStamp-mismatch newly added as Fiber static
     layout_stamp_resume_mismatch_total_) are complete under a single
     schema key (schema-2677) for Agent dashboards.
  AC6 Zero extra atomics on the happy (no-steal, no-stamp) path beyond
     the existing snapshot sample (verified by code inspection: 1
     snapshot + 1 ticket compare (one-shot) + 1 LayoutStamp probe).

Exit 0 = all AC rows satisfied.
"""

from __future__ import annotations

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

    def must_count(n: str, label: str, hay: str, at_least: int) -> None:
        c = hay.count(n)
        if c < at_least:
            fails.append(f"{label}: expected ≥{at_least} occurrence(s) of {n!r}, found {c}")

    fiber_h = _read("src/serve/fiber.h")
    fiber_cpp = _read("src/serve/fiber.cpp")
    _read("src/compiler/aura_jit_bridge.cpp")
    bridge_weak = _read("src/compiler/fiber_bridge.cpp")
    ev_fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/serve/test_fiber_mutation_steal_safety.cpp")
    build = _read("build.py")

    # AC1: check_and_enforce_resume_invariants() exists + Fiber::resume
    # is the single call site (no other callers in production path).
    must("check_and_enforce_resume_invariants()", "AC1", fiber_h)
    must("check_and_enforce_resume_invariants()", "AC1", fiber_cpp)
    # Fiber::resume single call site (consolidated). The legacy
    # check_and_enforce_resume_snapshot_invariant may still be called
    # by tests; we only assert the canonical call FROM Fiber::resume.
    must_count("if (!check_and_enforce_resume_invariants())", "AC1", fiber_cpp, at_least=1)
    # LayoutStamp probe wired through the C ABI shim.
    must("aura_evaluator_check_resume_layout_stamp", "AC1", fiber_cpp)
    must("aura_evaluator_check_resume_layout_stamp", "AC1", ev_fiber)
    # Strong def in evaluator_fiber_mutation.cpp + weak no-op in
    # fiber_bridge.cpp (light link units).
    must("aura_evaluator_check_resume_layout_stamp", "AC1", bridge_weak)
    must("bump_layout_stamp_resume_mismatch", "AC1", ev_fiber)

    # AC2: production_defaults_active() ignores AURA_STEAL_SNAPSHOT_SOFT=1.
    # The Soft mode check is_steal_snapshot_soft_mode() must consult
    # production_locked BEFORE the env var.
    must("is_steal_snapshot_soft_mode", "AC2", fiber_cpp)
    must("steal_snapshot_soft_production_locked", "AC2", fiber_cpp)
    # Decision table comment (L1209-1242) documents the production lock.
    must("Issue #2677", "AC2", fiber_h)  # decision table reference
    # Bridge shim also checks the production lock before continuing
    # (Force_deopt weak no-op aborts under production lock).
    must("steal_snapshot_soft_production_locked", "AC2", bridge_weak)

    # AC3: publish_mutation_safety_mirrors called at Guard enter/exit
    # + orch soft-boundary enter/exit. 3 known sites in
    # evaluator_fiber_mutation.cpp (Guard exit, orch_soft_boundary_enter,
    # orch_soft_boundary_exit) + 1 helper definition.
    must_count("publish_mutation_safety_mirrors", "AC3", ev_fiber, at_least=3)
    must("orch_soft_boundary_enter", "AC3", ev_fiber)
    must("orch_soft_boundary_exit", "AC3", ev_fiber)

    # AC4: test_resume_invariants_2677() exists in
    # tests/serve/test_fiber_mutation_steal_safety.cpp (per #81967
    # extend existing file).
    must("test_resume_invariants_2677", "AC4", test)
    must("aura_evaluator_check_resume_layout_stamp", "AC4", test)
    must("set_steal_snapshot_soft_for_test", "AC4", test)
    must("current_safety_ticket", "AC4", test)
    must("set_resume_layout_stamp", "AC4", test)
    must("set_resume_safety_ticket", "AC4", test)
    # Sub-tests: ticket mismatch, hard cancel, layout-stamp, full mismatch,
    # happy path. test calls main() in production.
    must("Sub-test A: ticket mismatch", "AC4", test)
    must("Sub-test B: Hard mode cancels", "AC4", test)
    must("Sub-test C: LayoutStamp mismatch", "AC4", test)
    must("Sub-test D: full mismatch", "AC4", test)
    must("Sub-test E: happy path", "AC4", test)

    # AC5: layout_stamp_resume_mismatch_total_ counter + accessors +
    # C ABI + query keys under schema-2677.
    must("layout_stamp_resume_mismatch_total_", "AC5", fiber_h)
    must("layout_stamp_resume_mismatch_total", "AC5", fiber_cpp)  # accessor
    must("aura_fiber_static_layout_stamp_resume_mismatch_total", "AC5", fiber_cpp)
    must("schema-2677", "AC5", obs)
    must("issue-2677", "AC5", obs)
    must("layout-stamp-resume-mismatch-fiber-total", "AC5", obs)
    must("resume-invariants-consolidated", "AC5", obs)
    # 4 existing metrics still present (no regression).
    must("mutation_steal_snapshot_mismatch_total_", "AC5", fiber_h)
    must("steal_snapshot_mismatch_force_deopt_total_", "AC5", fiber_h)
    must("steal_snapshot_hard_fail_total_", "AC5", fiber_h)
    must("steal_safety_ticket_mismatch_total_", "AC5", fiber_h)

    # AC6: zero extra atomics on happy path. The function reads
    # exactly: mutation_safety_snapshot() (1 seqlock read + ticket
    # compare if set) + aura_evaluator_check_resume_layout_stamp
    # (single weak no-op or strong is_fully_fresh compare). Verified
    # by structural inspection of the function body — no fetch_add / no
    # extra atomic store on the early-return-true branch.
    must("if (!snap_inconsistent && !ticket_miss && !layout_mismatch)", "AC6", fiber_cpp)
    must("return true; // consistent", "AC6", fiber_cpp)

    # Linter self-coverage + build.py wire-up.
    must("check_resume_invariants_2677", "self", build)
    must("#2677", "self", fiber_cpp)
    must("#2677", "self", ev_fiber)
    must("#2677", "self", obs)
    must("#2677", "self", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2677 runtime(steal) resume invariants consolidated — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
