#!/usr/bin/env python3
"""Issue #3419: JIT typed-entry commit_readiness on every compiled function.

Interpreter gates execute / execute_function / run_function. JIT prologue
only emitted the call for named functions, linear probe skipped state==0,
and weak stubs could bind OK. Residual of #3224/#3379.

Contract:
  AC1 Every production/Full JIT function entry (anonymous included) emits
      aura_jit_ir_typed_entry_commit_readiness_ok and deopts on 0
  AC2 ABI selfcheck treats stub typed-entry as fail (strong marker, bit 8)
  AC3 Soft/Off omit the call
  AC4 no new query key; reuse elide / post-mutate rollback counters
  AC5 extend persist-rehydrate + steal-complete; linter after #3343; no invent

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

    jit = _read("src/compiler/aura_jit.cpp")
    tma = _read("src/compiler/typed_mutation_audit.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    brc = _read("src/compiler/aura_jit_bridge.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fb = _read("src/compiler/fiber_bridge.cpp")
    rah = _read("src/serve/runtime_production_abi.h")
    rab = _read("src/serve/runtime_production_abi.cpp")
    occ = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    steal = _read("tests/serve/test_steal_complete_strong_entry.cpp")
    build = _read("build.py")

    must("kJitTypedEntryEveryFunctionIssue = 3419", "AC1 stamp", tma)
    must("hard_typed_entry", "AC1 production gate", jit)
    must("can_typed", "AC1 typed emit", jit)
    must('prologue_name = named ? fn.name : "<anon>"', "AC1 anonymous", jit)
    must("fn_ir_typed_entry_commit_readiness_ok", "AC1 emit call", jit)
    must("fn_deopt_to_interpreter", "AC1 deopt", jit)
    must("can_epoch || can_typed", "AC1 prologue without name", jit)

    must("aura_abi_strong_ir_typed_entry_v", "AC2 stub cites marker", stub)
    if 'extern "C" __attribute__((weak, used)) int aura_abi_strong_ir_typed_entry_v' in stub:
        fails.append("AC2: stub must not define aura_abi_strong_ir_typed_entry_v (cross-DSO weak wins over strong T)")
    must("aura_abi_strong_ir_typed_entry_v", "AC2 strong marker", fm)
    must("aura_abi_strong_ir_typed_entry_v", "AC2 fiber weak", fb)
    must("kProductionAbiSelfcheckFailBitTypedEntry", "AC2 fail bit", rah)
    must("aura_abi_strong_ir_typed_entry_v() == 0", "AC2 selfcheck", rab)
    must('extern "C" int aura_abi_strong_ir_typed_entry_v(void) noexcept', "AC2 strong def", fm)
    if (
        "return 1"
        not in fm[fm.find("aura_abi_strong_ir_typed_entry_v") : fm.find("aura_abi_strong_ir_typed_entry_v") + 220]
    ):
        fails.append("AC2: strong marker must return 1")
    must("ir_typed_entry_commit_readiness_ok", "AC2 wrapper in jit_bridge", brc)

    pro = jit.find("Issue #3419: production/Full also emit typed-entry")
    pro_win = jit[pro : pro + 3500] if pro >= 0 else ""
    must("hard_typed_entry", "AC3 Soft omit", pro_win)
    must("production_defaults_active()", "AC3 production load", pro_win)
    must("AuditStrategy::Full", "AC3 Full", pro_win)

    if "schema-3419" in stub or "schema-3419" in rah or "schema-3419" in tma:
        fails.append("AC4: new schema-3419 query key (forbidden)")
    if "g_3419_" in stub or "g_3419_" in jit or "g_3419_" in tma:
        fails.append("AC4: new g_3419_* counter (forbidden)")
    must("g_linear_fast_path_elide_blocked_production_total", "AC4 reuse elide", jit)
    must("linear_post_mutate_force_rollback_total", "AC4 reuse rollback", jit)

    must("ac3419_jit_typed_entry_every_function", "AC5 persist-rehydrate", occ)
    must("3419 AC2: fail bit 8", "AC5 steal suite", steal)
    must("check_jit_typed_entry_every_function_3419", "AC5 build.py", build)
    prev = build.find("check_production_weak_abi_commit_readiness_3343")
    ours = build.find("check_jit_typed_entry_every_function_3419")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3419 linter must run after #3343")
    if (ROOT / "tests" / "compiler" / "test_issue_3419.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3419.cpp present (forbidden)")
    if (ROOT / "tests" / "issues" / "test_issue_3419.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3419.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3419-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("check_jit_typed_entry_every_function_3419: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3419 JIT typed-entry every function — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
