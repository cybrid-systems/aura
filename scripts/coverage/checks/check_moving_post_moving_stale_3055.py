#!/usr/bin/env python3
"""Issue #3055: post-Moving last_object_remap_ residual.

Contract (one row per AC):
  AC1  After objects_moved>0 + pin_contract_held, slot/pin/RootRemap
       cover known remapped roots; residual canary is observe-only.
  AC2  Unregistered live ptr still holding a remap key → incomplete +
       pin_contract_held=false + sticky (same shape).
  AC3  Soft / no-move / Moving disabled: no extra walk.
  AC4  RootRemapPass + slot + pin remain the only remap mechanisms.
  AC5  EnvFrame steal×compact stays hold-depth + scan_skip_freed.
  AC6  Extend fail_closed + coverage_gate; no test_issue_3055.cpp;
       no docs/design/ (#1655).

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

    arena = _read("src/core/arena.ixx")
    dc = _read("src/core/densify_consistency_report.h")
    efl = _read("src/core/envframe_lifetime.ixx")
    ev = _read("src/compiler/evaluator.ixx")
    health = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    gate = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    build = _read("build.py")

    must("kMovingPostMovingStaleIssue = 3055", "AC1 stamp", dc)
    must("note_post_moving_live_ptr_canary", "AC1 canary", arena)
    must("count_post_moving_stale_known_ptrs_", "AC1 scan", arena)
    must("ac3055_1_slot_covered_no_canary_holds", "AC1 test", test)

    must("ac3055_2_unregistered_canary_fail_closed", "AC2 test", test)
    must("post_moving_stale_count", "AC2 result field", arena)
    must("g_moving_post_moving_stale_total", "AC2 counter", dc)

    must("ac3055_3_soft_no_scan", "AC3 test", test)
    must("moved_live_objects && !last_object_remap_.empty()", "AC3 gate", arena)

    must("ac3055_4_no_second_remap", "AC4 test", test)
    must("invoke_root_remap_callback_", "AC4 RootRemap", arena)
    must("remap_pins_pointing_to", "AC4 pin", arena)
    if "class PostMovingPinRegistry" in arena or "g_moving_pin_registry_3055" in arena:
        fails.append("AC4: second pin registry introduced")
    must("not a second remap", "AC4 evaluator", ev)

    must("scan_skip_freed", "AC5", efl)
    must("Issue #3055", "AC5 envframe", efl)
    must("ac3055_5_envframe_hold_depth_unchanged", "AC5 test", test)

    must("ac3055_6_source_cite_no_invent", "AC6 test", test)
    must("schema-3055", "AC6 health", health)
    must("3055", "AC6 gate", gate)
    must("check_moving_post_moving_stale_3055", "AC6 build", build)
    if _read("docs/design/3055-post-moving-stale.md"):
        fails.append("AC6: docs/design/3055-* present")
    if _read("tests/core/test_issue_3055.cpp"):
        fails.append("AC6: test_issue_3055.cpp present")

    if fails:
        print(f"Issue #3055 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3055 post-Moving last_object_remap_ residual — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
