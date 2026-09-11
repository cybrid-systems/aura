#!/usr/bin/env python3
"""Issue #3653: Full audit / proof-gate before outermost persist freeze.

#3614 hoisted linear + drain before persist. #3517 flips Guard success
after Full deny. Persist still froze Occurrence / Stamped before
exit_mutation_boundary's Full audit, so steal / query:type / IR could
observe a green face for one beat.

Contract (one row per AC):
  AC1  Production/Full + outermost audit deny: check-only composite /
       proof-gate / finish_mutate_hard_gate sit BEFORE persist; deny
       reuses the #3472 un-stamp set (no freeze)
  AC2  #3614 linear deny still precedes persist; #3440 consume + #3545
       undo + #3517 consume retained; exactly 3 #3158 restore sites;
       no second restore helper
  AC3  happy path: sole snapshot writer remains the persist helper
  AC4  Soft/Off: gate is production/Full-only (no extra audit walk)
  AC5  extends test_type_linear_commit_health + persist-rehydrate;
       linter AFTER #3666; no test_issue_3653.cpp; no docs/design/;
       no query:type-linear-commit-health / evolution-snapshot rewrite

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _count(hay: str, needle: str) -> int:
    n = 0
    p = hay.find(needle)
    while p != -1:
        n += 1
        p = hay.find(needle, p + 1)
    return n


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    dtor = _read("src/compiler/evaluator_mutation_boundary.cpp")
    health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    occ = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qr = _read("src/compiler/evaluator_primitives_query_reflect.cpp")

    gate_3614 = dtor.find("Issue #3614")
    gate_3653 = dtor.find("Issue #3653")
    persist_call = dtor.find("aura_outermost_success_persist_occurrence(ev_")
    consume_3440 = dtor.find("consume_outermost_persist_reject_needs_restore()")
    belt_3472 = dtor.find("Issue #3472")
    exit_pos = dtor.find("ev_->exit_mutation_boundary(success)")
    consume_3517 = dtor.find("consume_outermost_audit_rollback_needs_fail()")

    win = dtor[gate_3653:persist_call] if gate_3653 >= 0 and persist_call > gate_3653 else ""

    must("Issue #3653", "AC1 Guard cite", dtor)
    if not (gate_3653 >= 0 and persist_call > gate_3653):
        fails.append("AC1: #3653 gate does not precede persist")
    must("boundary_solve_proof_gate", "AC1 proof-gate", win)
    must("finish_mutate_hard_gate", "AC1 hard-gate", win)
    must("composite_txn_commit", "AC1 composite", win)
    must("production_defaults_active()", "AC1 production face", win)
    must("get_strategy() == typed_audit::AuditStrategy::Full", "AC1 Full face", win)
    must("clear_type_linear_commit_proof_on_abort()", "AC1 deny clear proof", win)
    must("kTypeLinearProofOutcomeReject", "AC1 deny publish", win)
    must("aura_clear_occurrence_persist_buffer(ev_)", "AC1 deny clear persist", win)
    must("clear_type_export_authority", "AC1 deny clear grant", win)
    must("ac3653_1_audit_deny_pre_persist_no_write", "AC1 runtime", health)

    if not (
        gate_3614 >= 0
        and gate_3653 > gate_3614
        and persist_call > gate_3653
        and consume_3440 > persist_call
        and belt_3472 > consume_3440
        and exit_pos > belt_3472
        and consume_3517 > exit_pos
    ):
        fails.append("AC2: order #3614 → #3653 → persist → #3440 → #3472 → exit → #3517 broken")
    must("note_outermost_persist_reject_needs_restore", "AC2 3440 note", dtor)
    must("undo_apply_coercion_map_recent", "AC2 3545 undo", dtor)
    must("consume_outermost_audit_rollback_needs_fail", "AC2 3517 consume", dtor)
    if _count(dtor, "restore_or_clear_occurrence_to_entry(") != 3:
        fails.append("AC2: #3158 restore site count != 3")
    must_not("abort_restore_3653", "AC2 no second restore", dtor)
    must_not("abort_restore_dual_topology_3653", "AC2 no second restore (full)", dtor)
    must("ac3653_2_source_order_audit_before_persist", "AC2 health order", health)
    must("drain_pending_full_solve_before_commit", "AC2 3614 drain kept", dtor)
    must("enforce_linear_boundary_consistency", "AC2 3614 walk kept", dtor)

    if _count(dtor, "note_occurrence_commit_snapshot_written(") != 1:
        fails.append("AC3: sole snapshot writer count != 1")
    must("ac3653_3_happy_persist_stamped", "AC3 happy", health)
    must("finish_mutate_hard_gate", "AC3 hard-gate impl", tc)
    must("boundary_solve_proof_gate", "AC3 proof-gate impl", tc)
    must("stage_expected_occurrence_snapshot_fp", "AC3 composite stages fp", tc)

    must("ac3653_4_soft_zero_cost_no_extra_walk", "AC4 Soft", health)
    must("production_defaults_active()", "AC4 Soft skip face", win)

    must("check_hot_contract_production_pack_3666", "AC5 prev linter", build)
    must("check_outermost_persist_audit_order_3653", "AC5 build.py", build)
    prev = build.find("check_hot_contract_production_pack_3666")
    ours = build.find("check_outermost_persist_audit_order_3653")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3666")
    must("ac3653_audit_before_persist_source", "AC5 persist-rehydrate", occ)
    must("query:type-linear-commit-health", "AC5 health key retained", qr)
    must("query:type-linear-evolution-snapshot", "AC5 evolution key retained", qr)
    must_not("schema-3653", "AC5 no new query key", q + qr)
    if "query:type-linear-commit-health" in win or "query:type-linear-evolution-snapshot" in win:
        fails.append("AC5: rewrote old query:type-linear-* key")
    if _read("tests/compiler/test_issue_3653.cpp"):
        fails.append("AC5: test_issue_3653.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3653-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3653 outermost_persist_audit_order:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3653 outermost_persist_audit_order")
    return 0


if __name__ == "__main__":
    sys.exit(main())
