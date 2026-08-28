#!/usr/bin/env python3
"""Issue #3224: production IR/JIT entry refuses under !commit_readiness.

Move/Drop already consult linear_move_drop_elision_ok (#3130/#3186).
Other typed ops (Borrow/MutBorrow/CastOp/Apply) could still run under
active mutation when would_allow_commit is false. Gate execute /
call_closure / execute_function and JIT Apply prologue.

Contract:
  AC1 Production + active mutation + !would_allow → refuse / deopt
  AC2 Move/Drop still gated by linear_move_drop_elision_ok
  AC3 Soft / quiet (depth==0) zero extra commit_readiness
  AC4 Extend test_occurrence_goal_persist_rehydrate; linter; no invent /
      docs/design/3224-*; no new counters

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    ir = _read("src/compiler/ir_executor_impl.cpp")
    jit = _read("src/compiler/aura_jit.cpp")
    brh = _read("src/compiler/aura_jit_bridge.h")
    brc = _read("src/compiler/aura_jit_bridge.cpp")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    must("kIrTypedEntryCommitReadinessIssue", "AC1 stamp", tma)
    must("ir_typed_entry_commit_readiness_ok", "AC1 predicate", tma)
    must("aura_evaluator_mutation_boundary_depth()", "AC1 depth", tma)
    must("commit_readiness(commit_readiness_live_policy())", "AC1 readiness", tma)
    must("ir_typed_entry_blocked_result", "AC1 IR helper", ir)
    must("commit-readiness-refused", "AC1 refuse", ir)
    must("IRInterpreter::execute()", "AC1 execute", ir)
    must("IRInterpreter::call_closure", "AC1 call_closure", ir)
    must("IRInterpreter::execute_function", "AC1 execute_function", ir)
    must("aura_jit_ir_typed_entry_commit_readiness_ok", "AC1 JIT ABI", brh)
    must("typed_audit::ir_typed_entry_commit_readiness_ok()", "AC1 JIT wrap", brc)
    must("fn_ir_typed_entry_commit_readiness_ok", "AC1 JIT fn", jit)
    must("Issue #3224", "AC1 prologue cite", jit)
    must("CreateICmpEQ(entry_ok_i, zero32)", "AC1 blocked", jit)
    must("aura_jit_ir_typed_entry_commit_readiness_ok", "AC1 stub", stub)

    must("linear_move_drop_elision_ok()", "AC2 IR Move/Drop", ir)

    pred = tma.find("ir_typed_entry_commit_readiness_ok() noexcept")
    pred_win = tma[pred : pred + 2800] if pred >= 0 else ""
    must("production_defaults_active()", "AC3 Soft gate", pred_win)
    must("if (depth == 0)", "AC3 quiet", pred_win)
    must("aura_evaluator_mutation_boundary_depth()", "AC3 depth ABI", pred_win)
    must("ac3224_ir_typed_entry_commit_readiness", "AC3 test", t)

    must("g_linear_fast_path_elide_blocked_production_total", "AC4 reuse", tma)
    must("check_ir_typed_entry_commit_readiness_3224", "AC4 build.py", build)
    if "g_3224_" in tma or "g_3224_" in ir:
        fails.append("AC4: new g_3224_* counter (reuse existing)")
    if "query:ir-typed-entry" in tma:
        fails.append("AC4: new query:* (forbidden)")

    if (ROOT / "tests" / "issues" / "test_issue_3224.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3224.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3224.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3224.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3224-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    if fails:
        print("FAIL #3224 ir_typed_entry_commit_readiness:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3224 ir_typed_entry_commit_readiness: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
