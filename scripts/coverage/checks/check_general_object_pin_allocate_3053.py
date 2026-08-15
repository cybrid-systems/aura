#!/usr/bin/env python3
"""Issue #3053: allocate / pool+flat residual on required pin cover.

Contract (one row per AC):
  AC1  try_allocate / allocate_checked / create share allocate_raw_impl
       auto-wire; required + unpinned allocate blocks live_compact(Moving)
       with pin_contract_held=false + sticky densify-off.
  AC2  value-only register_external_root_for_densify is still not cover.
  AC3  Soft / pref<=0 / render: single required_active load, no inventory.
  AC4  auto_wire + EXEMPT remain SSOT; kGeneralObjectPinAdoptSiteCount stays 7.
  AC5  Extend test_moving_densify_fail_closed + coverage_gate (#81967).
  AC6  No second pin registry; no Soft cost regression.

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

    lp = _read("src/core/lifetime_pin.hh")
    arena = _read("src/core/arena.ixx")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    gate = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    build = _read("build.py")

    must("kGeneralObjectPinAllocateResidualIssue = 3053", "AC1 stamp", lp)
    must("maybe_note_allocate_intermediate_", "AC1 helper", arena)
    must("Issue #3053", "AC1 arena", arena)
    must("ac3053_1_allocate_path_auto_wires_and_blocks", "AC1 test", test)

    must("ac3053_2_value_only_still_not_cover", "AC2 test", test)
    must("value-only", "AC2 comment", arena)

    must("general_object_pin_required_active()", "AC3 load", arena)
    must("ac3053_3_soft_allocate_zero_cost", "AC3 test", test)

    must("kGeneralObjectPinAdoptSiteCount = 7", "AC4 floor", lp)
    must("do not bump", "AC4 no hand-bump", lp)

    must("ac3053_4_mutate_densify_allocate_soak", "AC5 soak", test)
    must("3053", "AC5 coverage-gate", gate)
    must("check_general_object_pin_allocate_3053", "AC5 build", build)

    if "g_moving_pin_registry_3053" in arena or "class DensifyPinRegistry" in arena:
        fails.append("AC6: second pin registry introduced")
    if _read("docs/design/3053-pin-allocate-residual.md"):
        fails.append("AC6: docs/design/3053-* present")
    if _read("tests/core/test_issue_3053.cpp"):
        fails.append("AC6: test_issue_3053.cpp present")

    if fails:
        print(f"Issue #3053 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3053 allocate residual pin cover — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
