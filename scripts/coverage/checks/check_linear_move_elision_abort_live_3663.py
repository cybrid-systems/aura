#!/usr/bin/env python3
"""Issue #3663: Production Move opcode deletion consults live elision_ok.

#3591 wired aura_linear_fast_path_ok into lowering Move elision (epoch
arm). That predicate reads last_proof_would_allow_commit — abort can
already be in-flight while the last proof is still green. Once MoveOp
is deleted, the #3446 IR/JIT executor gate never sees it.

Contract (one row per AC):
  AC1  Production else-if ORs aura_jit_linear_move_drop_elision_ok()==0
  AC2  #3591 ok() and #3519 depth/blocked-escape still emit first
  AC3  Soft skip via production probe; #2263 clean elide kept
  AC4  Drop always emits DropOp; IR executor AND / JIT #3446 OR unchanged
  AC5  extend escape_move_elision suite; linter AFTER #3662; no invent;
       no docs/design; no new query key / counter

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    lin = _read("src/compiler/lowering_linear_types_impl.cpp")
    gate = _read("src/compiler/ownership_escape_lowering_gate.h")
    test = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    jit = _read("src/compiler/aura_jit.cpp")
    ir = _read("src/compiler/ir_executor_impl.cpp")
    build = _read("build.py")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    qh = _read("src/compiler/evaluator_primitives_query.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")

    move = lin.find("case aura::ast::NodeTag::Move:")
    borrow = lin.find("case aura::ast::NodeTag::Borrow:")
    move_win = ""
    if move >= 0:
        move_win = lin[move:borrow] if borrow > move else lin[move : move + 9000]

    must("Issue #3663", "AC1 cite", move_win)
    must("aura_jit_linear_move_drop_elision_ok()", "AC1 elision_ok ABI", move_win)
    must("aura_linear_fast_path_ok() == 0", "AC1 #3591 ok() kept", move_win)
    must(
        "aura_production_defaults_active_probe() != 0",
        "AC1 production probe",
        move_win,
    )
    # Production conjunct is OR of the two == 0 (abort/live participate).
    if "aura_linear_fast_path_ok() == 0 ||" not in move_win.replace("\n", " "):
        # tolerate wrapping
        compact = " ".join(move_win.split())
        if "aura_linear_fast_path_ok() == 0 || aura_jit_linear_move_drop_elision_ok() == 0" not in compact:
            fails.append("AC1: Production else-if must OR ok()==0 with elision_ok()==0")

    must("kLinearMoveElisionAbortLiveIssue = 3663", "AC1 stamp", gate)
    must("aura_jit_linear_move_drop_elision_ok", "AC1 gate ABI decl", gate)
    must("ac3663_1_abort_in_flight_emits", "AC1 test", test)

    must("Issue #3591", "AC2 #3591", move_win)
    must("Issue #3519", "AC2 #3519", move_win)
    must("aura_linear_fast_path_depth_or_densify_block()", "AC2 depth", move_win)
    must("escape_blocks_move_elision_for_current", "AC2 blocked-escape", move_win)
    must("ac3663_2_3591_3519_still_emit", "AC2 test", test)
    p3519 = move_win.find("Issue #3519")
    p3663 = move_win.find("Issue #3663")
    if p3519 < 0 or p3663 < 0 or p3519 > p3663:
        fails.append("AC2: #3519 depth arm must stay before #3663 conjunct")

    must("ac3663_3_soft_still_elides", "AC3 test", test)
    must("#2263 clean", "AC3 Soft #2263", move_win)
    must("elide unchanged", "AC3 Soft elide kept", move_win)

    drop = lin.find("case aura::ast::NodeTag::Drop:")
    drop_win = lin[drop : drop + 900] if drop >= 0 else ""
    must("IROpcode::DropOp", "AC4 Drop emit", drop_win)
    must_not("aura_jit_linear_move_drop_elision_ok", "AC4 Drop no elision_ok", drop_win)
    must_not("linear_move_elided", "AC4 Drop no opcode elide", drop_win)
    must_not(
        "aura_jit_ir_typed_entry_commit_readiness_ok",
        "AC4 no typed-entry OR into lowering allow",
        lin,
    )
    must("linear_move_drop_elision_ok()", "AC4 IR executor AND", ir)
    must(
        "is_stale = irb->CreateOr(is_stale, fence_elision_blocked)",
        "AC4 JIT #3446 OR elision",
        jit,
    )
    must(
        "is_stale = irb->CreateOr(is_stale, fence_entry_blocked)",
        "AC4 JIT #3446 OR typed-entry",
        jit,
    )
    must("ac3663_4_drop_and_executor_unchanged", "AC4 test", test)

    must("check_is_coercible_dynamic_prod_3662", "AC5 prev linter", build)
    must("check_linear_move_elision_abort_live_3663", "AC5 build.py", build)
    prev = build.find("check_is_coercible_dynamic_prod_3662")
    ours = build.find("check_linear_move_elision_abort_live_3663")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3662")
    must("ac3663_5_suite_linter_no_invent", "AC5 test", test)
    must("query:type-linear-commit-health", "AC5 commit-health key", qh + obs)
    must("query:type-linear-evolution-snapshot", "AC5 evolution-snapshot key", qh + obs)
    must_not("schema-3663", "AC5 no new query key", lin + mut + qh + obs)
    if _read("tests/compiler/test_issue_3663.cpp") or _read("tests/issues/test_issue_3663.cpp"):
        fails.append("AC5: test_issue_3663.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3663-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3663 linear_move_elision_abort_live:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3663 linear_move_elision_abort_live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
