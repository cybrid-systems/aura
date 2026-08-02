#!/usr/bin/env python3
"""Issue #2495: Moving densify fail-closed on untracked external roots.

ASTArena::live_compact(Moving) only densifies small-pool tracked create
objects via relocate_tracked_objects_for_moving_. Non-small-pool /
untracked external raw pointers never enter last_object_remap_, leaving
Agents to treat Moving success + pin_contract_held as full pointer
safety while untracked live buffers still point at densified-away addresses.

Contract:
  AC1 LiveCompactResult exposes moving_incomplete_remap + untracked_kept_count
     + sets pin_contract_held=false when objects_moved > 0 && untracked_kept > 0
  AC2 All live roots pinned or root-remapped → contract held
     (covered by existing #2166 tests)
  AC3 Soft / no objects moved → zero extra work (gated conditional)
  AC4 Query / stats surface: g_moving_untracked_external_roots_total +
     LiveCompactResult.{moving_incomplete_remap, untracked_kept_count}
  AC5 Source-cite registrations + tests + linter

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    ixx = _read("src/core/arena.ixx")
    test = _read("tests/core/test_moving_densify_fail_closed_2495.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — LiveCompactResult fields + pin_contract_held = false on
    # incomplete remap.
    must("Issue #2495", "AC1", ixx)
    must("moving_incomplete_remap", "AC1", ixx)
    must("untracked_kept_count", "AC1", ixx)
    must("relocate_tracked_objects_for_moving_(&untracked_kept_local)", "AC1", ixx)
    must(
        "if (result.objects_moved > 0 && result.untracked_kept_count > 0)",
        "AC1",
        ixx,
    )
    must("result.moving_incomplete_remap = true", "AC1", ixx)
    must("result.pin_contract_held = false", "AC1", ixx)

    # AC3 — Soft / no-objects-moved remains zero-cost.
    must("untracked_kept_count == 0", "AC3", ixx)

    # AC4 — query / stats surface.
    must("g_moving_untracked_external_roots_total", "AC4", ixx)
    must(
        "g_moving_untracked_external_roots_total.fetch_add",
        "AC4",
        ixx,
    )
    must("g_moving_untracked_hard_abort_pref", "AC4", ixx)

    # AC5 — registrations + test ac functions + this linter.
    must("ac1_source_cite_live_compact_result", "AC1", test)
    must("ac3_soft_zero_extra_work", "AC3", test)
    must("ac4_query_stats_surface", "AC4", test)
    must("ac5_source_and_gate", "AC5", test)
    must("Issue #2495", "AC5", test)
    must("test_moving_densify_fail_closed_2495", "AC5", cmake)
    must(
        "aura_add_issue_test(test_moving_densify_fail_closed_2495)",
        "AC5",
        cmake,
    )
    must(
        "aura_issue_test_link_light(test_moving_densify_fail_closed_2495)",
        "AC5",
        cmake,
    )
    must("check_moving_densify_fail_closed_2495", "AC5", build)
    must("cmd_moving_densify_fail_closed_2495_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2495 moving densify fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
