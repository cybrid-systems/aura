#!/usr/bin/env python3
"""Issue #3617: steal residual hard-AND keys Lifetime/EnvFrame on victim eval.

#2957's LifetimeProofOk + #2745's EnvFrameOk arms read the process-wide
last-proof / last-densify atomics: one evaluator's densify Reject frozen
steal for the whole pool (multi-tenant over-reject), while fibers without
a Guard-entered identity escaped the arm entirely. #2727 already had the
per-Fiber durable evaluator_id — the arms now read per-eval keyed slots
(lock-free, no steal-decision mutex) instead of process-last.

Contract:
  AC1 LifetimeProofOk arm reads victim-eval keyed slots: present_for +
      would_allow_for + last_densify_call_seq_for; per-eval LCP slot
      table in lifetime_consistency_proof.hh
  AC2 EnvFrameOk arm reads victim-eval keyed densify slots; boundary
      bump site mirrors the fresh result into note_last_densify_result_for
  AC3 gates unchanged: (hard || latched) stays on Lifetime; null identity
      → quiet skip; no new gate on EnvFrame beyond keyed seq
  AC4 reuse StealInvariant bits + existing reject counters; no new
      metric name / query key / g_3617_* counter
  AC5 test extends test_steal_complete_gc_defer.cpp (built member of
      test_mailbox_fiber_batch); no new test file; no docs/design
  AC6 source-cite evaluate_residual_hard_and_bits +
      aura_fiber_evaluator_id_for_steal_safety; identity mirror wired in
      MutationBoundaryGuard enter/dtor; build.py wires this linter after
      the #3616 linter

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ss = _read("src/serve/steal_safety.cpp")
    lcp = _read("src/core/lifetime_consistency_proof.hh")
    dens = _read("src/core/densify_consistency_report.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    arena = _read("src/core/arena.ixx")
    hooks = _read("src/core/gc_hooks.h")
    test = _read("tests/serve/test_steal_complete_gc_defer.cpp")
    build = _read("build.py")

    # AC1 — Lifetime arm keyed + LCP per-eval slots.
    must(
        "void* victim_eval_id = aura_fiber_evaluator_id_for_steal_safety(stolen)",
        "AC1 victim identity",
        ss,
    )
    must("last_lifetime_consistency_proof_present_for(victim_eval_id)", "AC1 keyed present", ss)
    must("!lcp::last_lifetime_consistency_would_allow_for(victim_eval_id)", "AC1 keyed allow", ss)
    must("g_lcp_eval_slots", "AC1 slot table", lcp)
    must("stamp_lifetime_consistency_proof_for", "AC1 keyed stamp", lcp)
    must("last_lifetime_consistency_would_allow_for", "AC1 keyed query", lcp)

    # AC2 — EnvFrame arm keyed + densify mirror.
    must("last_densify_envframe_ok_for(victim_eval_id)", "AC2 keyed envframe", ss)
    must("last_densify_dual_epoch_ok_for(victim_eval_id)", "AC2 keyed dual epoch", ss)
    must("last_densify_call_seq_for(victim_eval_id)", "AC2 keyed seq", ss)
    must("note_last_densify_result_for", "AC2 densify mirror", dens)
    must("note_last_densify_result_for", "AC2 bump-site mirror", mb)

    # AC3 — gates unchanged; null identity skips.
    must(
        "is_steal_snapshot_hard_mode() || aura_runtime_multi_worker_production_latched()",
        "AC3 hard||latched gate",
        ss,
    )
    if "skip(StealInvariant::EnvFrameOk) &&" in ss and "victim_eval_id" not in ss:
        fails.append("AC3: EnvFrame arm lost the victim identity read")

    # AC4 — no new counters / query keys; existing bits reused.
    if "g_3617_" in ss or "g_3617_" in lcp or "g_3617_" in dens:
        fails.append("AC4: new g_3617_* counter (forbidden)")
    if "schema-3617" in ss:
        fails.append("AC4: new schema-3617 query key (forbidden)")
    must("steal_invariant_mask(StealInvariant::EnvFrameOk)", "AC4 EnvFrame bit", ss)
    must("steal_invariant_mask(StealInvariant::LifetimeProofOk)", "AC4 Lifetime bit", ss)

    # Structural (windowed, not bare substring): the keyed reads must sit
    # INSIDE the Lifetime arm — gate + identity + three keyed reads in one
    # bounded region right after the arm comment.
    arm = ss.find("StealInvariant::LifetimeProofOk — Issue #2957 residual arm")
    arm_win = ss[arm : arm + 1700] if arm >= 0 else ""
    must(
        "last_lifetime_consistency_proof_present_for(victim_eval_id)",
        "AC1 keyed present inside Lifetime arm window",
        arm_win,
    )
    must("last_densify_call_seq_for(victim_eval_id)", "AC3 keyed seq inside Lifetime arm window", arm_win)
    must(
        "is_steal_snapshot_hard_mode() || aura_runtime_multi_worker_production_latched()",
        "AC3 hard||latched gate inside Lifetime arm window",
        arm_win,
    )

    # AC5 — test extension, no invent, no docs.
    must("ac3617_eval_keyed_residual", "AC5 gc_defer test", test)
    if (ROOT / "tests" / "serve" / "test_issue_3617.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_3617.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3617-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    # AC6 — source-cite + identity mirror + build.py wiring order.
    must("evaluate_residual_hard_and_bits", "AC6 source-cite", test)
    must("aura_fiber_evaluator_id_for_steal_safety(stolen)", "AC6 identity source-cite", test)
    must("set_current_eval_identity", "AC6 Guard-enter mirror", mb)
    must("g_current_eval_identity", "AC6 TLS", hooks)
    must("stamp_lifetime_consistency_proof_for", "AC6 arena keyed stamp", arena)
    must("stamp_lifetime_consistency_proof_for", "AC6 steal-complete keyed stamp", efm)
    must("check_steal_eval_keyed_residual_3617", "AC6 build.py", build)
    prev = build.find("check_jit_anon_linear_prologue_3616")
    ours = build.find("check_steal_eval_keyed_residual_3617")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: #3617 linter must run after #3616")

    if fails:
        print("check_steal_eval_keyed_residual_3617: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3617 steal eval-keyed residual — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
