#!/usr/bin/env python3
"""Issue #3857 source-cite gate: Moving entry soft-gate on live temp canaries.

#3210 TemporaryMovingLivePtrCanary is observe-only and the live_compact
(Moving) precondition gate is TLS-only (arena_mutation_boundary_depth);
the apply thread never enters MutationBoundary, so a peer fiber's
apply_closure window (cl_copy.flat / cl_copy.pool stack copies) cannot
block a concurrent densify relocate — mid-apply × Moving UAF.

ACs:
  AC1  live_compact(Moving) entry soft-gates when the process-wide #3210
       canary inventory is live: one acquire load on g_inventory.live →
       moving_blocked_precondition + soft_gated + additive counter, before
       the #3210 drain (gate cannot be bypassed by the drain path).
  AC2  post-relocate late-window re-drain sits between the relocate call
       and the #3055/#3182 stale scan; a late-noted canary whose object
       was relocated fail-closes the window; both additive counters are
       stamped at densify_consistency_report.h end.
  AC3  apply_closure canaries remain observe-only (#3210 shape intact) and
       no second model appears (no new pin registry); no strays
       (no tests/**/test_issue_3857.cpp, no docs/design/3857-*).
  AC4  test_moving_densify_fail_closed.cpp drives the #3857 ACs
       (ac3857_1..ac3857_5 defined AND dispatched from the runner).
  AC5  build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARENA = ROOT / "src" / "core" / "arena.ixx"
DC = ROOT / "src" / "core" / "densify_consistency_report.h"
APPLY = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
TST = ROOT / "tests" / "core" / "test_moving_densify_fail_closed.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def main() -> int:
    arena = ARENA.read_text() if ARENA.exists() else ""
    dc = DC.read_text() if DC.exists() else ""
    apply_src = APPLY.read_text() if APPLY.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1: entry soft-gate on the process-wide inventory, before the drain.
    gate_idx = arena.find("moving_temp_canary_detail::g_inventory.live.load(")
    drain_idx = arena.find("note_temporary_moving_live_canaries();")
    gate_span = arena[gate_idx : gate_idx + 2200] if gate_idx != -1 else ""
    good = (
        "Issue #3857" in arena
        and gate_idx != -1
        and drain_idx != -1
        and gate_idx < drain_idx
        and "std::memory_order_acquire) > 0" in gate_span
        and "result.moving_blocked_precondition = true;" in gate_span
        and "result.soft_gated = true;" in gate_span
        and "g_moving_blocked_temp_canary_total" in gate_span
        and "return result;" in gate_span
    )
    report("AC1", good, "entry soft-gate reads g_inventory.live before the #3210 drain")

    # AC2: late-window re-drain feeds the existing stale gate; counters at
    # header end.
    relocate_idx = arena.find("relocate_tracked_objects_for_moving_(&untracked_kept_local)")
    redrain_idx = arena.find("g_moving_late_temp_canary_total")
    stale_idx = arena.find("count_post_moving_stale_known_ptrs_(this_window_remap)")
    good = (
        relocate_idx != -1
        and redrain_idx != -1
        and stale_idx != -1
        and relocate_idx < redrain_idx < stale_idx
        and "note_temporary_moving_live_canaries();" in arena[redrain_idx : redrain_idx + 400]
        and "g_moving_blocked_temp_canary_total{0}" in dc
        and "g_moving_late_temp_canary_total{0}" in dc
        and "kMovingTempCanaryEntryGateIssue = 3857" in dc
        and "kMovingTempCanaryLateRedrainIssue = 3857" in dc
    )
    report("AC2", good, "re-drain between relocate and stale scan; counters stamped")

    # AC3: observe-only shape intact, no second model, no strays.
    no_second_model = "class PostMovingPinRegistry" not in arena and "g_moving_pin_registry_3857" not in arena
    no_stray = (
        not (ROOT / "tests" / "core" / "test_issue_3857.cpp").exists()
        and not (ROOT / "docs" / "design" / "3857-0.md").exists()
    )
    good = (
        "TemporaryMovingLivePtrCanary tmp_flat" in apply_src
        and "TemporaryMovingLivePtrCanary tmp_pool" in apply_src
        and "observe-only" in apply_src
        and no_second_model
        and no_stray
    )
    report("AC3", good, "apply_closure canaries observe-only; no second model; no strays")

    # AC4: the densify-pin batch drives block + release + soak.
    good = all(
        tst.count(fn) >= 2
        for fn in (
            "ac3857_1_entry_gate_blocks_then_releases",
            "ac3857_2_soft_off_unchanged",
            "ac3857_3_late_redrain_wiring",
            "ac3857_4_soak_block_release_compose",
            "ac3857_5_source_cite_no_invent",
        )
    )
    report("AC4", good, "test defines and dispatches ac3857_1..5")

    # AC5: gate wiring — build.py runs the linter and the allowlist lists it.
    wired = "check_temp_canary_moving_gate_3857.py" in build and ("check_temp_canary_moving_gate_3857.py" in allow)
    report("AC5", wired, "build.py + root_check_allowlist.txt wired")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
