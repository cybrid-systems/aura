#!/usr/bin/env python3
"""Issue #2499: unify RootRemapPass fail with pin_contract_held (single Moving success gate).

#2294 / #2365 / #2368 RootRemapPass writes per-call fail totals into
LiveCompactResult.root_remap_*_fail_total. Phase 5 in
evaluator_mutation_boundary.cpp gates on compact_r.pin_contract_held only
— Agents see "pin ok + root_remap fail cumulative" mixed signal.

This linter enforces that:
  AC1 Phase 5 in evaluator_mutation_boundary.cpp ANDs
       (compact_r.root_remap_*_fail_total == 0) into pin_contract_held —
       fail-closed shape same as #2266 pin_contract_held + #2497
       scan_fail_delta.
  AC2 AdaptiveCompactResult exposes root_remap_stable_ref_fail_total +
       root_remap_closure_capture_fail_total aggregates (sum across
       arenas) for the driver.
  AC3 Soft densify path: no RootRemap work → fail counters default zero
       → pin_contract_held unchanged (zero extra cost).
  AC4 Query keys additive (root_remap_stable_ref_fail_total +
       root_remap_closure_capture_fail_total + last_root_remap_any_fail
       retained). Last-call vs cumulative documented in arena.ixx
       comment (per-call out-params from invoke_root_remap_callback_).
  AC5 Source-cite Issue #2499 in arena.ixx + evaluator_mutation_boundary.cpp
       + root_remap_pass.ixx + linter self-test.

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

    arx = _read("src/core/arena.ixx")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    rpx = _read("src/compiler/root_remap_pass.ixx")
    test = _read("tests/compiler/test_root_remap_pin_contract_unified_2499.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — Phase 5 ANDs root_remap fail totals into pin_contract_held +
    # live_compact folds at densify source (single Moving success gate).
    must("pin_contract_held = compact_r.pin_contract_held", "AC1", emb)
    must("Issue #2499", "AC1", emb)
    must("compact_r.root_remap_stable_ref_fail_total == 0", "AC1", emb)
    must("compact_r.root_remap_closure_capture_fail_total == 0", "AC1", emb)
    must("densify_root_remap_call_ok", "AC1", emb)
    must("densify_pin_axis_ok", "AC1", emb)
    # densify source fold after invoke_root_remap_callback_.
    must("invoke_root_remap_callback_", "AC1", arx)
    must("result.pin_contract_held = false", "AC1", arx)
    must("Issue #2499", "AC1", arx)

    # AC2 — AdaptiveCompactResult aggregates root_remap fail totals.
    must("AdaptiveCompactResult", "AC2", arx)
    must("root_remap_stable_ref_fail_total", "AC2", arx)
    must("root_remap_closure_capture_fail_total", "AC2", arx)
    # Aggregation in compact_all_moving_pinned.
    must("compact_all_moving_pinned", "AC2", arx)
    must("out.root_remap_stable_ref_fail_total +=", "AC2", arx)
    must("out.root_remap_closure_capture_fail_total +=", "AC2", arx)
    # Re-AND aggregate fail totals into pin_contract_held.
    must("out.pin_contract_held = false", "AC2", arx)
    # LiveCompactResult retains the per-arena fields (no regression).
    must("LiveCompactResult", "AC2", arx)
    must("std::size_t root_remap_stable_ref_fail_total = 0", "AC2", arx)
    must("std::size_t root_remap_closure_capture_fail_total = 0", "AC2", arx)

    # AC3 — Soft densify zero-cost (fail counters default zero).
    must("default true", "AC3", arx)  # pin_contract_held default
    must("last-call semantics", "AC3", arx)  # not process-cumulative
    # Phase 5 Soft branch (no Moving) leaves pin_contract_held untouched.
    must("if (had_moving_densify && pin_contract_held)", "AC3", emb)

    # AC4 — Query keys additive + last-call vs cumulative documented.
    must("root_remap_stable_ref_fail_total", "AC4", rpx)
    must("root_remap_closure_capture_fail_total", "AC4", rpx)
    must("last_root_remap_any_fail", "AC4", rpx)  # last-call accessor retained
    must("per-call", "AC4", arx)
    must("#2376", "AC4", arx)

    # AC5 — Source-cite + helper + registrations.
    must("Issue #2499", "AC5", arx)
    must("Issue #2499", "AC5", emb)
    must("Issue #2499", "AC5", test)
    must("AC5", "AC5", test)
    must("aura_add_issue_test(test_root_remap_pin_contract_unified_2499)", "AC5", cmake)
    must("aura_issue_test_link_light(test_root_remap_pin_contract_unified_2499)", "AC5", cmake)
    must("add_dependencies(all_test_issue_targets test_root_remap_pin_contract_unified_2499)", "AC5", cmake)
    must("check_root_remap_pin_contract_unified_2499", "AC5", build)

    if fails:
        print("check_root_remap_pin_contract_unified_2499: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_root_remap_pin_contract_unified_2499: OK (5/5 AC rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
