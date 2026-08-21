#!/usr/bin/env python3
"""Issue #3219: production facade C-ABI joint dual-writes Evaluator/core.

After #3150 closed C-ABI joint (g_current_bridge_epoch /
g_aot_defuse_version / g_aot_table_epoch), production
mark_define_dirty / invalidate_function still early-return after the
facade and skip atomic_bump_epochs_and_stamp_bridge. Evaluator
defuse_version_ and core WorkspaceEpoch stay behind; is_bridge_stale /
is_env_frame_stale can miss MustDeopt while aura_is_jit_closure_fresh
already flipped.

Fix: after facade success, CompilerService dual-writes Evaluator/core
via stamp_eval_core_joint_after_production_facade_ (same mutate_mtx_).
Does not re-bump aura_aot_bump_func_table_epoch (owner-scoped #2951).
Soft/Off: facade returns false; helper never runs (zero extra).

Contract (one row per AC):
  AC1  helper bumps Evaluator defuse + core bridge + stamp/expire;
       mark_define_dirty / invalidate_function call it after facade
  AC2  helper lives inside facade-success branch only (Soft/Off zero extra)
  AC3  helper does not re-bump aura_aot_bump_func_table_epoch
  AC4  existing #3112 / #3129 / #3150 / #3188 cites preserved; no new query:*
  AC5  test_compiler_hot_update_facade ac3219_*; this linter in build.py;
       no docs/design/3219-*; no test_issue_3219.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _fn_win(src: str, sig: str) -> str:
    pos = src.find(sig)
    if pos < 0:
        return ""
    nxt = src.find("\nvoid CompilerService::", pos + 1)
    return src[pos:nxt] if nxt > pos else src[pos : pos + 12000]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ixx = _read("src/compiler/service.ixx")
    svc = _read("src/compiler/service_dirty.cpp")
    hur = _read("src/compiler/hot_update_registry.cpp")
    t = _read("tests/compiler/test_compiler_hot_update_facade.cpp")
    build = _read("build.py")

    hpos = ixx.find("void stamp_eval_core_joint_after_production_facade_")
    hwin = ixx[hpos : hpos + 2200] if hpos >= 0 else ""
    must("Issue #3219", "AC1 helper cite", ixx)
    must("evaluator_.bump_defuse_version_for_test()", "AC1 Evaluator defuse", hwin)
    must("bump_bridge_epoch()", "AC1 core bridge", hwin)
    must("expire_stale_live_closures_", "AC1 expire", hwin)
    must("notify_walk_active_closures_", "AC1 walk", hwin)
    must("on_typed_mutation_epoch_bump()", "AC1 solve_delta wipe", hwin)

    md = _fn_win(svc, "void CompilerService::mark_define_dirty")
    inv = _fn_win(svc, "void CompilerService::invalidate_function")
    must("Issue #3219", "AC1 mark_define_dirty cite", md)
    must("stamp_eval_core_joint_after_production_facade_(name)", "AC1 mark_define_dirty helper", md)
    must("Issue #3219", "AC1 invalidate_function cite", inv)
    must(
        "stamp_eval_core_joint_after_production_facade_(name)",
        "AC1 invalidate_function helper",
        inv,
    )

    facade_md = md.find("hard_invalidate_via_facade(")
    helper_md = md.find("stamp_eval_core_joint_after_production_facade_(name)")
    soft_md = md.find("gc_coord::Scope gc_coord_scope")
    if facade_md < 0 or helper_md < 0 or helper_md < facade_md:
        fails.append("AC2: helper must follow facade success in mark_define_dirty")
    if soft_md < 0 or not (facade_md < helper_md < soft_md):
        fails.append("AC2: helper must sit inside facade-success branch (before Soft body)")

    if "aura_aot_bump_func_table_epoch" in hwin:
        fails.append("AC3: helper re-bumps AOT table epoch (forbidden; owner-scoped #2951)")

    must("Issue #3112", "AC4 #3112 facade", hur)
    must("aura_aot_bump_func_table_epoch()", "AC4 #3129 table epoch", hur)
    must("aura_hot_update_bump_bridge_epoch()", "AC4 #3150 C bridge", hur)
    must("aura_hot_update_bump_defuse_version()", "AC4 #3150 C defuse", hur)
    must("notify_dirty_define(name)", "AC4 #3150 dirty", hur)
    must("Issue #3188 AC1: residual of #3150", "AC4 #3188 IR/shape", md)
    must("Issue #3219", "AC4 facade 3219 cite", hur)
    if "query:eval-core-joint" in svc or "query:eval-core-joint" in hur:
        fails.append("AC4: new query:* name (reuse existing epochs)")

    must("ac3219_eval_core_joint_after_production_facade", "AC5 test fn", t)
    must("check_eval_core_joint_after_production_facade_3219", "AC5 build.py", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3219.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3219.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3219.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3219.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3219-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3219 eval_core_joint_after_production_facade:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3219 eval_core_joint_after_production_facade: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
