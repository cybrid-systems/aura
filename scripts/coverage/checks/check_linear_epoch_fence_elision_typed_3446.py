#!/usr/bin/env python3
"""Issue #3446: JIT Move/Drop epoch fence ORs live elision_ok + typed-entry.

#3186/#3224/#3419 wired those predicates into linear_safety_probe and the
Apply prologue. The probe deopt_inc then continues to bb_ok. Compiled
Move/Drop still only skipped the body on epoch-stale, so a reject proof
after a green prologue still copied + zeroed the source slot.

Contract:
  AC1 fence ORs elision_ok==0 + typed_entry==0; Move body stays after fence
  AC2 Drop/Borrow/MutBorrow use the same fence
  AC3 interpreter still linear_state_allows_op then elision skip (#3224/#3305)
  AC4 Apply prologue (#3419) and GuardShape probe i1 fail-close unchanged
  AC5 Soft/Off helpers return allow; no new query key / metric name

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    jit = (ROOT / "src" / "compiler" / "aura_jit.cpp").read_text()
    ir = (ROOT / "src" / "compiler" / "ir_executor_impl.cpp").read_text()
    tma = (ROOT / "src" / "compiler" / "typed_mutation_audit.h").read_text()
    test = (ROOT / "tests" / "compiler" / "test_escape_move_elision_gate.cpp").read_text()
    occ = (ROOT / "tests" / "compiler" / "test_occurrence_goal_persist_rehydrate.cpp").read_text()
    mutate = (ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp").read_text()
    build = (ROOT / "build.py").read_text()

    must("Issue #3446", "AC1 marker", jit)
    fence = jit.find("auto begin_linear_epoch_fence")
    if fence < 0:
        fails.append("AC1: begin_linear_epoch_fence missing")
        body = ""
    else:
        body = jit[fence : fence + 3200]
    must("Issue #3446", "AC1 fence cite", body)
    must("fn_linear_move_drop_elision_ok", "AC1 fence elision call", body)
    must("fn_ir_typed_entry_commit_readiness_ok", "AC1 fence typed-entry call", body)
    must(
        "fence_elision_blocked = irb->CreateICmpEQ(fence_elision_ok_i, zero32)",
        "AC1 elision == 0",
        body,
    )
    must(
        "fence_entry_blocked = irb->CreateICmpEQ(fence_entry_ok_i, zero32)",
        "AC1 typed-entry == 0",
        body,
    )
    must("is_stale = irb->CreateOr(is_stale, fence_elision_blocked)", "AC1 OR elision", body)
    must("is_stale = irb->CreateOr(is_stale, fence_entry_blocked)", "AC1 OR typed-entry", body)
    must("irb->CreateCondBr(is_stale, bb_stale, bb_ok)", "AC1 stale skip body", body)
    must("irb->CreateBr(bb_merge)", "AC1 stale merge skip", body)
    must_not("linear_safety_probe()", "AC1 fence must not route through probe", body)
    must("3446 AC1", "AC1 test", test)

    move = jit.find("case OpMoveOp:")
    move_body = jit[move : move + 2800] if move >= 0 else ""
    must("begin_linear_epoch_fence()", "AC1 Move uses fence", move_body)
    must("store(inst.ops[1], c64(0))", "AC1 source zero after fence", move_body)
    must("Issue #3446", "AC1 Move cite", move_body)
    must_not("linear_safety_probe()", "AC1 Move not probe", move_body)

    drop = jit.find("case OpDropOp:")
    drop_body = jit[drop : drop + 1200] if drop >= 0 else ""
    must("begin_linear_epoch_fence()", "AC2 Drop uses fence", drop_body)
    must("Issue #3446", "AC2 Drop cite", drop_body)
    borrow = jit.find("case OpBorrowOp:")
    borrow_body = jit[borrow : borrow + 800] if borrow >= 0 else ""
    must("begin_linear_epoch_fence()", "AC2 Borrow/MutBorrow inherit fence", borrow_body)
    must("3446 AC2", "AC2 test", test)

    must("linear_state_allows_op", "AC3 interpreter state gate", ir)
    must("typed_audit::linear_move_drop_elision_ok()", "AC3 interpreter elision", ir)
    must("ir_typed_entry_blocked_result", "AC3 interpreter typed-entry", ir)
    must("3446 AC3", "AC3 test", test)

    must("Issue #3419", "AC4 prologue kept", jit)
    must("hard_typed_entry", "AC4 prologue gate", jit)
    must("is_stale = irb->CreateOr(is_stale, lin_unsafe)", "AC4 GuardShape probe i1", jit)
    must("3446 AC4", "AC4 test", test)

    must("g_linear_fast_path_elide_blocked_production_total", "AC5 reuse counter", tma)
    must_not("schema-3446", "AC5 no new query key", mutate)
    must_not("schema-3446", "AC5 no schema in jit", jit)
    must("3446 AC5", "AC5 test", test)
    must("check_linear_epoch_fence_elision_typed_3446", "AC5 build.py", build)
    must("ac3446_linear_epoch_fence_elision_typed", "AC5 occurrence", occ)
    prev = build.find("check_jit_typed_entry_every_function_3419")
    ours = build.find("check_linear_epoch_fence_elision_typed_3446")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3446 linter must run after #3419")
    if (ROOT / "tests" / "compiler" / "test_issue_3446.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3446.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3446.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3446.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3446-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3446 linear_epoch_fence_elision_typed:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3446 linear_epoch_fence_elision_typed: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
