#!/usr/bin/env python3
"""Issue #2971: production-required GeneralObjectPin on create + densify.

Contract (one row per AC):
  AC1  production default still locks required (step 15) + ASTArena::create
       auto-wires intermediates via note_intermediate_create_auto_wire_
       / note_general_object_create_auto_wire (densify-visible via #2775)
  AC2  required + unpinned intermediates block live_compact(Moving) BEFORE
       relocate_tracked_objects_for_moving_; pin_contract_held=false
  AC3  #2837/#2935 slot-covered intermediates skip the pre-move block
  AC4  additive schema keys on densify-health / lifetime-pin-stats /
       live-compact-stats: auto-wire, required-enforced, breach-densify-fail,
       pre-move-unpinned-block + schema-2971
  AC5  Soft / unset: create is a single required_active load (no inventory)
  AC6  tests in existing Moving suite + coverage linter + no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

FILES = [
    "src/core/lifetime_pin.hh",
    "src/core/arena.ixx",
    "src/compiler/security_defaults.hh",
    "src/compiler/evaluator_primitives_obs_eval.cpp",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "src/compiler/evaluator_primitives_stdlib_review.cpp",
    "tests/core/test_moving_densify_fail_closed.cpp",
    "scripts/coverage/checks/check_general_object_pin_create_densify_2971.py",
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

    lp = _read("src/core/lifetime_pin.hh")
    arena = _read("src/core/arena.ixx")
    hh = _read("src/compiler/security_defaults.hh")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    health = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    pinq = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # ── AC1: production default + create auto-wire ──
    must("Issue #2971", "AC1 lifetime_pin", lp)
    must("kGeneralObjectPinCreateDensifyIssue = 2971", "AC1 stamp", lp)
    must("note_general_object_create_auto_wire", "AC1 helper", lp)
    must("#2971", "AC1 security_defaults", hh)
    must("g_general_object_pin_required_pref.store(1, std::memory_order_release)", "AC1 production lock", hh)
    must("note_intermediate_create_auto_wire_", "AC1 create auto-wire", arena)
    must("note_general_object_create_auto_wire", "AC1 create helper call", arena)
    must("register_external_root_for_densify", "AC1 densify-visible", arena)
    must("in_render_hotpath", "AC1 render exempt", arena)

    # ── AC2: pre-move densify gate ──
    must("has_unpinned_intermediate_creates_", "AC2 helper", arena)
    must("relocate_tracked_objects_for_moving_", "AC2 relocate still present", arena)
    must("g_general_object_pin_pre_move_unpinned_block_total", "AC2 counter", lp)
    must("g_general_object_pin_required_breach.store", "AC2 sticky breach", arena)
    must("pin_contract_held = false", "AC2 pin_contract_held", arena)
    # Gate must appear BEFORE the relocate *call* in source order.
    gate_pos = arena.find("has_unpinned_intermediate_creates_()")
    reloc_pos = arena.find("result.objects_moved = relocate_tracked_objects_for_moving_")
    if gate_pos == -1 or reloc_pos == -1 or gate_pos > reloc_pos:
        fails.append("AC2: pre-move gate must precede relocate_tracked_objects_for_moving_ call")
    must("ac2971_2_pre_move_densify_gate", "AC2 test", test)

    # ── AC3: slot-covered allows densify ──
    must("external_root_slots_for_densify_", "AC3 slots", arena)
    must("collect_pinned_ptrs_for_arena", "AC3 pin coverage", lp)
    must("ac2971_3_slot_covered_allows_densify", "AC3 test", test)

    # ── AC4: additive observability ──
    must("schema-2971", "AC4 live-compact-stats", obs)
    must("general-object-pin-pre-move-unpinned-block-total", "AC4 live-compact-stats", obs)
    must("general-object-pin-auto-wire-total", "AC4 auto-wire preserved", obs)
    must("general-object-pin-required-enforced-total", "AC4 enforced preserved", obs)
    must("general-object-pin-required-breach-densify-fail-total", "AC4 breach-fail preserved", obs)
    must("schema-2971", "AC4 densify-health", health)
    must("general-object-pin-auto-wire-total", "AC4 densify-health auto-wire", health)
    must("general-object-pin-required-enforced-total", "AC4 densify-health enforced", health)
    must("general-object-pin-required-breach-densify-fail-total", "AC4 densify-health breach-fail", health)
    must("schema-2971", "AC4 lifetime-pin-stats", pinq)
    must("general-object-pin-auto-wire-total", "AC4 lifetime-pin-stats auto-wire", pinq)
    must("schema-2709", "AC4 lineage 2709", obs)
    must("schema-2840", "AC4 lineage 2840", obs)

    # ── AC5: Soft zero cost ──
    must("general_object_pin_required_active()", "AC5 create gate", arena)
    must("ac2971_5_soft_zero_cost", "AC5 test", test)

    # ── AC6: tests + build + no design ──
    for rel in FILES:
        content = _read(rel)
        if not content:
            fails.append(f"AC6: missing file {rel}")
            continue
        if "2971" not in content and "Issue #2971" not in content:
            fails.append(f"AC6: {rel} does not cite 2971")
    must("ac2971_1_production_default_and_create_auto_wire", "AC6", test)
    must("ac2971_2_pre_move_densify_gate", "AC6", test)
    must("ac2971_3_slot_covered_allows_densify", "AC6", test)
    must("ac2971_4_observability_schema", "AC6", test)
    must("ac2971_5_soft_zero_cost", "AC6", test)
    must("ac2971_6_linter_and_no_design", "AC6", test)
    must("check_general_object_pin_create_densify_2971", "AC6 build", build)
    must("cmd_general_object_pin_create_densify_2971", "AC6 build cmd", build)
    design_docs = sorted((ROOT / "docs" / "design").glob("2971-*")) if (ROOT / "docs" / "design").is_dir() else []
    if design_docs:
        fails.append(f"AC6: docs/design/2971-* present ({[p.name for p in design_docs]})")
    if (ROOT / "tests" / "core" / "test_issue_2971.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_2971.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_general_object_pin_create_densify_2971: {len(fails)} failure(s)")
        return 1

    print("check_general_object_pin_create_densify_2971: OK (AC1-AC6)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
