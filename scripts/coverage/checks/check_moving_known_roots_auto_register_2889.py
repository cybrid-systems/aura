#!/usr/bin/env python3
"""Issue #2889: Moving densify known-roots auto-register coverage linter.

Contract (one row per AC):
  AC1  densify entry walk (inside moving_compact_enabled() block) auto-
       registers known intermediate slots + RootRemap compiler roots via
       ArenaGroup::register_external_root_slot_for_densify_all BEFORE
       compact_all_moving_pinned (source-cite)
  AC2  additive counter g_moving_known_roots_auto_registered_total +
       read accessor + test reset; #2749 split counters preserved
  AC3  truly foreign pointers stay unregistered → fail-closed preserved
       (incomplete_remap + pin_contract_held=false + sticky densify-off)
  AC4  query:arena-moving-densify-health additive keys
       (known-roots-auto-registered-total + #2749 splits) + schema-2889;
       existing #2837 / #2619 / #2495 keys preserved
  AC5  source-cite + tests in existing src/-aligned Moving suite
       (tests/core/test_moving_densify_fail_closed.cpp, #81967);
       no docs/design/ per #1655

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

FILES = [
    "src/core/arena.ixx",
    "src/core/densify_consistency_report.h",
    "src/compiler/root_remap_pass.ixx",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "tests/core/test_moving_densify_fail_closed.cpp",
    "scripts/coverage/checks/check_moving_known_roots_auto_register_2889.py",
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

    arena = _read("src/core/arena.ixx")
    report = _read("src/core/densify_consistency_report.h")
    rrp = _read("src/compiler/root_remap_pass.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # ── AC1: densify entry auto-register walk ──
    must("Issue #2889", "AC1", mb)
    must("register_external_root_slot_for_densify_all", "AC1", mb)
    must("root_remap_registered_slots_snapshot", "AC1", mb)
    must("compact_all_moving_pinned", "AC1", mb)
    must("if (aura::ast::moving_compact_enabled())", "AC1", mb)
    must("register_external_root_slot_for_densify_all", "AC1", arena)
    must("root_remap_registered_slots_snapshot", "AC1", rrp)

    # ── AC2: additive counter + accessor + reset; #2749 preserved ──
    must("g_moving_known_roots_auto_registered_total", "AC2", report)
    must("kMovingKnownRootsAutoRegisterIssue = 2889", "AC2", report)
    must("moving_known_roots_auto_registered_total_v_read", "AC2", report)
    must("reset_moving_known_roots_auto_registered_for_test", "AC2", report)
    must("g_moving_auto_registered_remapped_total", "AC2", report)
    must("g_moving_still_untracked_incomplete_total", "AC2", report)

    # ── AC3: fail-closed for unknown roots preserved ──
    must("result.moving_incomplete_remap = true", "AC3", arena)
    must("result.pin_contract_held = false", "AC3", arena)
    must("g_moving_incomplete_remap_sticky_densify_off", "AC3", arena)
    must("densify_untracked_kept", "AC3", mb)

    # ── AC4: additive query keys + schema; existing preserved ──
    must("known-roots-auto-registered-total", "AC4", obs)
    must("auto-registered-remapped-total", "AC4", obs)
    must("still-untracked-incomplete-total", "AC4", obs)
    must("known-roots-auto-register-wired", "AC4", obs)
    must("schema-2889", "AC4", obs)
    must("issue-2889", "AC4", obs)
    must("schema-2837", "AC4", obs)
    must("schema-2619", "AC4", obs)
    must("schema-2495", "AC4", obs)

    # ── AC5: source-cite + src/-aligned suite + build.py gate ──
    for rel in FILES:
        content = _read(rel)
        if not content:
            fails.append(f"AC5: missing file {rel}")
            continue
        if "Issue #2889" not in content:
            fails.append(f"AC5: {rel} does not cite Issue #2889")
    must("ac2889_1_auto_register_walk_source", "AC5", test)
    must("ac2889_2_counter_additive", "AC5", test)
    must("ac2889_3_fail_closed_preserved", "AC5", test)
    must("ac2889_4_query_keys", "AC5", test)
    must("ac2889_5_linter_and_no_design", "AC5", test)
    must("check_moving_known_roots_auto_register_2889", "AC5", build)
    design_docs = sorted((ROOT / "docs" / "design").glob("2889-*")) if (ROOT / "docs" / "design").is_dir() else []
    if design_docs:
        fails.append(f"AC5: docs/design/2889-* present ({[p.name for p in design_docs]})")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_moving_known_roots_auto_register_2889: {len(fails)} failure(s)")
        return 1

    print("check_moving_known_roots_auto_register_2889: OK (AC1-AC5)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
