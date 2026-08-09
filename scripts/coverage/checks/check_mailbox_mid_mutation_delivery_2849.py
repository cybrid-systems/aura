#!/usr/bin/env python3
"""Issue #2849: mailbox delivery hard-gated by outermost MutationBoundary.

Production fail-closed residual of #2680/#2312/#2551: push / broadcast_fanout
must never enqueue a payload that can observe mid-mutation state of a shared
Evaluator. Delivery is allowed only after outermost MutationBoundaryGuard
exit (depth==0 && !held). Phase-5 Guard dtor is the sole reopen of the
deliverability window (drain_deferred_under_budget).

Contract (one row per AC):
  AC1 note_mailbox_deferred_under_boundary sole helper; push + fanout call it;
     always Backpressure under live boundary (never enqueue)
  AC2 Phase-5 outermost exit sole reopen (clear_recv_boundary_reject_window +
     drain_deferred_under_budget)
  AC3 under_boundary_* counters + Soft soft_observe / production hard faces
  AC4 schema-2849 query keys; #2680 lineage retained
  AC5 ac2849_* tests (push BP, after-exit deliverable, chaos-lite, source-cite)
  AC6 Soft never weakens gate; residual after budget still #2551 hard throttle

Exit 0 = all rows satisfied.
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

    mb = _read("src/serve/multi_fiber_mailbox.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fh = _read("src/serve/fiber.h")
    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    test = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    build = _read("build.py")
    l2680 = _read("scripts/coverage/checks/check_mailbox_boundary_interleave_2680.py")

    # AC1 — sole helper + push/fanout sites
    must("Issue #2849", "AC1", mb)
    must("note_mailbox_deferred_under_boundary", "AC1", mb)
    must_count("note_mailbox_deferred_under_boundary", "AC1 def+sites", mb, 3)
    must("if (note_mailbox_deferred_under_boundary(&local_stats_))", "AC1 call sites", mb)
    must_count(
        "if (note_mailbox_deferred_under_boundary(&local_stats_))",
        "AC1 push+fanout",
        mb,
        2,
    )
    must("return PushStatus::Backpressure", "AC1 BP", mb)
    must("aura_evaluator_mutation_boundary_depth() > 0", "AC1 depth", mb)
    must("aura_evaluator_mutation_boundary_held() != 0", "AC1 held", mb)
    must("never enqueue", "AC1 never enqueue", mb)

    # AC2 — Phase-5 sole reopen + process-wide held visibility
    must("#2849", "AC2", emb)
    must("drain_deferred_under_budget", "AC2", emb)
    must("clear_recv_boundary_reject_window", "AC2", emb)
    must("reopens the mailbox deliverability window", "AC2", emb)
    must("aura_process_mutation_boundary_held_enter", "AC2 process-wide", emb)
    must("aura_process_mutation_boundary_held_exit", "AC2 process-wide", emb)
    must("g_process_mutation_boundary_held_count", "AC2 process-wide", efm)
    must("aura_process_mutation_boundary_held_enter", "AC2 fiber.h", fh)
    must("cross-thread mailbox", "AC2 held C ABI", efm)

    # AC3 — under_boundary counters + Soft/hard faces
    must("mailbox_under_boundary_deferred_total", "AC3", mb)
    must("mailbox_under_boundary_deferred_hard_total", "AC3", mb)
    must("mailbox_under_boundary_deferred_soft_observe_total", "AC3", mb)
    must("is_mutate_mailbox_strict", "AC3", mb)
    must("soft_observe", "AC3 Soft", mb)
    must("mailbox_shared_evaluator_deferred_total", "AC3 lineage #2680", mb)

    # AC4 — schema query keys
    must("schema-2849", "AC4", msg)
    must("mailbox-under-boundary-deferred-total", "AC4", msg)
    must("mailbox-under-boundary-gate-wired", "AC4", msg)
    must("schema-2680", "AC4 lineage", msg)

    # AC5 — tests
    must("ac2849_1_push_under_guard_always_bp", "AC5", test)
    must("ac2849_2_after_exit_deliverable", "AC5", test)
    must("ac2849_3_chaos_no_mid_mutation_recv", "AC5", test)
    must("ac2849_4_source_cite", "AC5", test)
    must("ac2849_5_schema_query", "AC5", test)
    must("ac2849_6_soft_never_weakens", "AC5", test)
    must("Issue #2849", "AC5", test)
    must("mid-mutation", "AC5", test)

    # AC6 — Soft never weakens; residual #2551
    must("never weakens the gate", "AC6", mb)
    must("mailbox_hold_starvation_hard_total", "AC6 residual", mb)
    must("check_mailbox_mid_mutation_delivery_2849", "AC6", build)
    # #2680 linter still green under sole-helper refactor
    must("note_mailbox_deferred_under_boundary", "AC6 #2680 linter", l2680)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2849 mailbox mid-mutation delivery hard-gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
