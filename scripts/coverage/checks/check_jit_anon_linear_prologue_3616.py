#!/usr/bin/env python3
"""Issue #3616: anon JIT prologue emits linear_post_mutate_enforce.

#3419 gave anonymous production functions the typed-entry probe, but the
linear post-mutate enforce (#1540) stayed inside the named epoch arm —
anon applies ran the JIT body with no linear check after remount
last==0 / persist-reject hygiene. can_linear now ORs the same UINT32_MAX
env-hint probe into the shared prologue deopt, suppressed when can_epoch
already emitted it (named) — exactly one probe per function.

Contract:
  AC1 can_linear gate: hard_typed_entry && !can_epoch && probe fn && deopt;
      prologue gate includes can_linear
  AC2 anon arm emits aura_jit_linear_post_mutate_enforce(UINT32_MAX), OR'd
      into the shared is_unsafe deopt
  AC3 Soft/Off zero prologue calls (hard_typed_entry gates can_linear)
  AC4 named epoch arm probe unchanged; #3419 typed-entry gate unchanged;
      no new query key / counter
  AC5 build.py wires this linter (after #3419); test extends
      persist-rehydrate; no invented test file; no docs/design

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
    occ = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    arm = jit.find("Issue #3616: same probe the named epoch arm emits")
    arm_win = jit[arm : arm + 700] if arm >= 0 else ""
    must("const bool can_linear = hard_typed_entry && !can_epoch &&", "AC1 gate", jit)
    must("if (can_epoch || can_typed || can_linear) {", "AC1 prologue gate", jit)
    must("auto* env_max_lin =", "AC1 anon probe", arm_win or jit)
    must("0xFFFFFFFFu", "AC1 UINT32_MAX env hint", arm_win)
    must(
        "llvm::FunctionCallee(builder.fn_linear_post_mutate_enforce)",
        "AC1 emit call",
        arm_win,
    )
    must("CreateOr(is_unsafe, lin_unsafe)", "AC2 shared deopt", jit)
    must("can_linear = hard_typed_entry", "AC3 Soft/Off zero calls", jit)
    must("llvm::ArrayRef<llvm::Value*>{env_max}", "AC4 named epoch arm probe", jit)
    must(
        "can_typed = hard_typed_entry && builder.fn_ir_typed_entry_commit_readiness_ok",
        "AC4 #3419 gate unchanged",
        jit,
    )
    if "g_3616_" in jit:
        fails.append("AC4: new g_3616_* counter (forbidden)")

    must("ac3616_anon_linear_prologue_enforce", "AC5 persist-rehydrate", occ)
    must("check_jit_anon_linear_prologue_3616", "AC5 build.py", build)
    prev = build.find("check_jit_typed_entry_every_function_3419")
    ours = build.find("check_jit_anon_linear_prologue_3616")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3616 linter must run after #3419")
    if (ROOT / "tests" / "compiler" / "test_issue_3616.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3616.cpp present (forbidden)")
    if (ROOT / "tests" / "issues" / "test_issue_3616.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3616.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3616-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("check_jit_anon_linear_prologue_3616: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3616 anon linear prologue — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
