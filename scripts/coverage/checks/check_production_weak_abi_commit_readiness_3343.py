#!/usr/bin/env python3
"""Issue #3343: production weak-ABI stubs fail-closed on IR/linear commit_readiness
and fiber-steal linear probe.

Weak Soft-allow in aura_jit_bridge_stub.cpp / fiber_bridge.cpp can ship as the
production path when the strong TUs are not resolved. Production defaults
must refuse / abort; Soft / light-link keep zero-cost allow.

Contract (one row per AC):
  AC1  production_defaults / production lock: JIT stubs refuse (IR entry
       / elision blocked / post-mutate unsafe); weak probe_linear aborts;
       worker null-ref aborts
  AC2  production / full-JIT link the strong defs (aura_jit_bridge.cpp,
       evaluator_fiber_mutation.cpp); ABI marker bit 7 in self-check
  AC3  force !commit_readiness under production → IR refuse; steal-complete
       still runs probe + escape clear
  AC4  Soft / unit-test / light-link without production defaults keep weak
       allow / empty no-op
  AC5  extends test_occurrence_goal_persist_rehydrate +
       test_steal_complete_strong_entry; linter AFTER #3224; no
       test_issue_3343.cpp; no docs/design/; no schema-3343 / g_3343_*

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

    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    brc = _read("src/compiler/aura_jit_bridge.cpp")
    fb = _read("src/compiler/fiber_bridge.cpp")
    wc = _read("src/serve/worker.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    rah = _read("src/serve/runtime_production_abi.h")
    rab = _read("src/serve/runtime_production_abi.cpp")
    cmake = _read("CMakeLists.txt")
    occ = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    steal = _read("tests/serve/test_steal_complete_strong_entry.cpp")
    build = _read("build.py")

    # AC1 JIT stubs
    must("Issue #3343", "AC1 stub cite", stub)
    must("aura_production_defaults_active_probe", "AC1 stub probe", stub)
    must("stub_production_defaults_active", "AC1 stub helper", stub)
    cr = stub.find("aura_jit_ir_typed_entry_commit_readiness_ok")
    cr_win = stub[cr : cr + 450] if cr >= 0 else ""
    must("stub_production_defaults_active", "AC1 IR stub gate", cr_win)
    must("return 0", "AC1 IR stub refuse", cr_win)
    el = stub.find('extern "C" __attribute__((weak)) int aura_jit_linear_move_drop_elision_ok')
    el_win = stub[el : el + 400] if el >= 0 else ""
    must("return 0", "AC1 elision stub refuse", el_win)
    pm = stub.find('extern "C" __attribute__((weak)) int aura_jit_linear_post_mutate_enforce')
    pm_win = stub[pm : pm + 400] if pm >= 0 else ""
    must("return 1", "AC1 post-mutate stub unsafe", pm_win)

    # AC1 steal probe
    pr = fb.find("aura_evaluator_probe_linear_on_steal()")
    pr_win = fb[max(0, pr - 500) : pr + 900] if pr >= 0 else ""
    must("Issue #683 / #3343", "AC1 probe cite", fb)
    must("steal_snapshot_soft_production_locked", "AC1 probe lock", pr_win)
    must("std::abort()", "AC1 probe abort", pr_win)
    cp = wc.find("static inline void call_probe_linear_on_steal")
    cp_win = wc[cp : cp + 1100] if cp >= 0 else ""
    must("Issue #3343", "AC1 worker cite", cp_win)
    must("steal_snapshot_soft_production_locked", "AC1 worker lock", cp_win)
    must("std::abort()", "AC1 worker abort", cp_win)

    # AC2 strong defs + nm/link
    must("typed_audit::ir_typed_entry_commit_readiness_ok()", "AC2 strong IR", brc)
    must("typed_audit::linear_move_drop_elision_ok()", "AC2 strong elision", brc)
    must('extern "C" void aura_evaluator_probe_linear_on_steal()', "AC2 strong probe", fm)
    must("probe_and_repin_linear_on_steal", "AC2 probe body", fm)
    must("src/compiler/aura_jit_bridge.cpp", "AC2 cmake strong", cmake)
    must("Do NOT add aura_jit_bridge_stub.cpp here", "AC2 cmake no stub", cmake)
    must("kProductionAbiSelfcheckFailBitProbeLinear", "AC2 fail bit", rah)
    must("aura_abi_strong_probe_linear_on_steal_v", "AC2 header marker", rah)
    must("aura_abi_strong_probe_linear_on_steal_v", "AC2 weak marker", fb)
    must("aura_abi_strong_probe_linear_on_steal_v", "AC2 strong marker", fm)
    if "aura_abi_strong_probe_linear_on_steal_v() == 0" not in rab:
        fails.append("AC2: self-check missing probe-linear marker")

    # AC3
    must("ac3343_production_weak_abi_commit_readiness", "AC3 IR test", occ)
    must("note_escape_gate_clear_on_steal", "AC3 escape clear", fm)
    must("aura_evaluator_probe_linear_on_steal()", "AC3 steal-complete folds probe", fm)

    # AC4 Soft keep allow
    must("return 1", "AC4 Soft IR allow", cr_win)
    must("return 1", "AC4 Soft elision allow", el_win)
    must("return 0", "AC4 Soft post-mutate pass", pm_win)
    must("3343 AC4", "AC4 Soft test", occ)

    # AC5
    must("check_production_weak_abi_commit_readiness_3343", "AC5 build.py", build)
    must("Issue #3343", "AC5 steal test", steal)
    must("ac3343_production_weak_abi_commit_readiness", "AC5 occ test", occ)
    prev = build.find("check_ir_typed_entry_commit_readiness_3224")
    ours = build.find("check_production_weak_abi_commit_readiness_3343")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3224")
    if "schema-3343" in stub or "schema-3343" in fm or "schema-3343" in rab:
        fails.append("AC5: new schema-3343 query key")
    if "g_3343_" in stub or "g_3343_" in fm or "g_3343_" in rab:
        fails.append("AC5: new g_3343_* counter")
    if _read("tests/compiler/test_issue_3343.cpp"):
        fails.append("AC5: test_issue_3343.cpp present (forbidden #81967)")
    if _read("tests/serve/test_issue_3343.cpp"):
        fails.append("AC5: tests/serve/test_issue_3343.cpp present (forbidden #81967)")
    if _read("docs/design/3343-production-weak-abi-commit-readiness.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3343 production_weak_abi_commit_readiness:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3343 production_weak_abi_commit_readiness: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
