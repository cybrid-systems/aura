#!/usr/bin/env python3
"""Issue #2935: Moving densify known-root coverage + sticky-off recovery.

Contract (one row per AC):
  AC1  densify entry uses shared register_known_moving_densify_root_slots
       (full inventory: workspace/mutate-target/current flat+pool +
       WorkspaceTree layer slots + RootRemap) BEFORE compact_all_moving_pinned
  AC2  production hard still arms sticky densify-off; Soft never arms
  AC3  Agent recovery path (recover_moving_sticky_densify_off +
       arena:recover-moving-sticky-densify): re-register + clear sticky +
       optional one-shot Moving densify; success requires pin_contract_held
       + no incomplete remap when densify retried
  AC4  additive counters sticky-cleared-via-recovery-total +
       densify-retry-after-recovery-total + schema-2935 on densify-health;
       #2889 known-roots counter preserved
  AC5  Soft / Moving-off zero densify work preserved (register walk inside
       moving_compact_enabled; recovery densify gated)
  AC6  tests in existing Moving suite + coverage linter + no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

FILES = [
    "src/core/densify_consistency_report.h",
    "src/compiler/evaluator.ixx",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "src/compiler/evaluator_primitives_memory.cpp",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "tests/core/test_moving_densify_fail_closed.cpp",
    "scripts/coverage/checks/check_moving_known_roots_sticky_recovery_2935.py",
]


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

    report = _read("src/core/densify_consistency_report.h")
    ev = _read("src/compiler/evaluator.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mem = _read("src/compiler/evaluator_primitives_memory.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")
    arena = _read("src/core/arena.ixx")

    # ── AC1: full inventory + shared register helper ──
    must("Issue #2935", "AC1", mb)
    must("register_known_moving_densify_root_slots", "AC1 densify entry", mb)
    must("register_known_moving_densify_root_slots", "AC1 Evaluator", ev)
    must("WorkspaceTree", "AC1 residual inventory", mb)
    must("parent_flat_", "AC1 residual inventory", mb)
    must("parent_pool_", "AC1 residual inventory", mb)
    must("workspace_flat_", "AC1 inventory", mb)
    must("mutate_target_flat_", "AC1 inventory", mb)
    must("current_flat_", "AC1 inventory", mb)
    must("root_remap_registered_slots_snapshot", "AC1 RootRemap", mb)
    must("register_external_root_slot_for_densify_all", "AC1 register all", mb)
    must("if (aura::ast::moving_compact_enabled())", "AC1 gated", mb)

    # ── AC2: hard arms / Soft never ──
    must("hard_pref > 0", "AC2", arena)
    must("Soft (hard_pref <= 0) does not arm sticky", "AC2", arena)
    must("g_moving_incomplete_remap_sticky_densify_off", "AC2", arena)
    must("ac2935_2_hard_arms_soft_never_preserved", "AC2 test", test)

    # ── AC3: Agent recovery path ──
    must("recover_moving_sticky_densify_off", "AC3 method", mb)
    must("recover_moving_sticky_densify_off", "AC3 decl", ev)
    must("MovingStickyDensifyRecoveryResult", "AC3 result", ev)
    must("clear_moving_incomplete_remap_sticky_densify_off", "AC3 clear", mb)
    must("compact_all_moving_pinned", "AC3 densify retry", mb)
    must("arena:recover-moving-sticky-densify", "AC3 primitive", mem)
    must("g_moving_sticky_cleared_via_recovery_total", "AC3 counter bump", mb)
    must("g_moving_densify_retry_after_recovery_total", "AC3 counter bump", mb)
    must("pin_contract_held", "AC3 success gate", mb)

    # ── AC4: additive counters + schema ──
    must("g_moving_sticky_cleared_via_recovery_total", "AC4", report)
    must("g_moving_densify_retry_after_recovery_total", "AC4", report)
    must("kMovingStickyDensifyRecoveryIssue = 2935", "AC4", report)
    must("moving_sticky_cleared_via_recovery_total_v_read", "AC4", report)
    must("moving_densify_retry_after_recovery_total_v_read", "AC4", report)
    must("reset_moving_sticky_densify_recovery_for_test", "AC4", report)
    must("g_moving_known_roots_auto_registered_total", "AC4 #2889 preserved", report)
    must("sticky-cleared-via-recovery-total", "AC4 densify-health", obs)
    must("densify-retry-after-recovery-total", "AC4 densify-health", obs)
    must("sticky-recovery-wired", "AC4 densify-health", obs)
    must("schema-2935", "AC4 densify-health", obs)
    must("issue-2935", "AC4 densify-health", obs)
    must("schema-2889", "AC4 lineage", obs)
    must("schema-2905", "AC4 lineage", obs)
    must("schema-2837", "AC4 lineage", obs)
    must("schema-2935", "AC4 primitive", mem)

    # ── AC5: Soft / Moving-off zero work ──
    must("if (aura::ast::moving_compact_enabled())", "AC5 densify gate", mb)
    must("moving_compact_enabled()", "AC5 recovery densify gate", mb)

    # ── AC6: tests + build + no design ──
    for rel in FILES:
        content = _read(rel)
        if not content:
            fails.append(f"AC6: missing file {rel}")
            continue
        if "2935" not in content and "Issue #2935" not in content:
            fails.append(f"AC6: {rel} does not cite 2935")
    must("ac2935_1_full_inventory_and_shared_helper", "AC6", test)
    must("ac2935_2_hard_arms_soft_never_preserved", "AC6", test)
    must("ac2935_3_agent_recovery_path", "AC6", test)
    must("ac2935_4_additive_metrics_and_schema", "AC6", test)
    must("ac2935_5_soft_zero_work_and_moving_off", "AC6", test)
    must("ac2935_6_linter_and_no_design", "AC6", test)
    must("check_moving_known_roots_sticky_recovery_2935", "AC6 build", build)
    must("cmd_moving_known_roots_sticky_recovery_2935", "AC6 build cmd", build)
    design_docs = sorted((ROOT / "docs" / "design").glob("2935-*")) if (ROOT / "docs" / "design").is_dir() else []
    if design_docs:
        fails.append(f"AC6: docs/design/2935-* present ({[p.name for p in design_docs]})")
    if (ROOT / "tests" / "core" / "test_issue_2935.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_2935.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_moving_known_roots_sticky_recovery_2935: {len(fails)} failure(s)")
        return 1

    print("check_moving_known_roots_sticky_recovery_2935: OK (AC1-AC6)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
