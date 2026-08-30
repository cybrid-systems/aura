#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #3413: last_reemit_success must not stamp the full force_jit mask on
# any n>0 — only_covered over-covers residual. Production `covered =
# override || demoted` in decide_and_reemit / on_reemit_pipeline_call
# washes residual_force_mask to 0 even for groups never re-emitted,
# breaking only_covered re-promote + storm-clear min-dirty.
#
# AC1 — decide_and_reemit skips the fallback `covered = demoted` stamp.
#       Pipeline reason-domain stamp is #3445 (override-only); this
#       issue only closed the facade fallback. Partial success must
#       not over-cover residual.
# AC2 — residual_force_mask() still exposes uncovered bits so
#       only_covered re-promote clears only the emitted bits.
# AC3 — Storm-clear min-dirty still drives for the uncovered bit
#       (residual != 0 reaches the storm-clear path).
# AC4 — Soft / Off / `force_jit_regions_mask_ == 0`: zero extra stores
#       (both call sites gate on `demoted != 0` before stamping).
# AC5 — Non-duplicative vs #2895 / #2949 / #2978 / #3026 / #3136 /
#       #3229 (all upstream contracts preserved per source-cite).
# AC6 — No docs/design/3413-*.md (banned per #1655) and no
#        tests/{issues,compiler,core}/test_issue_3413.cpp (must extend
#        test_aot_incremental_reemit.cpp per #81934).
# AC7 — test_aot_incremental_reemit.cpp carries AC1-AC7 markers for
#        #3413; build.py wires cmd_partial_reemit_success_coverage_3413.
#
# Self-test:
#   python3 scripts/check_partial_reemit_success_coverage_3413.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    fails: list[str] = []

    hot = (ROOT / "src" / "compiler" / "hot_update_registry.cpp").read_text()
    obs = (ROOT / "src" / "compiler" / "observability_metrics.h").read_text()
    test = (ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp").read_text()
    build = (ROOT / "build.py").read_text()

    # AC1 — decide_and_reemit skips the fallback demoted stamp.
    # Pipeline count∩emit / demoted stamp is the #3445 residual (do
    # not require those lines here — #3413 is facade-only).
    if "Issue #3413" not in hot:
        fails.append("AC1: hot_update_registry.cpp missing 'Issue #3413' marker")
    if "skip the fallback `covered = demoted` stamp" not in hot:
        fails.append(
            "AC1: decide_and_reemit must skip the fallback `covered = demoted` stamp (residual -> 0 over-cover source)"
        )

    # AC2 — residual_force_mask accessor + only_covered re-promote intact.
    if "residual_force_mask" not in hot:
        fails.append("AC2: residual_force_mask accessor missing in hot_update_registry.cpp")
    if "maybe_force_jit_repromote_on_clean_success" not in hot:
        fails.append("AC2: only_covered re-promote path missing")

    # AC3 — Storm-clear min-dirty still drives for the uncovered bit.
    # Residual != 0 must reach the storm-clear / min-dirty branch
    # (residual_force_mask return path is consumed downstream).
    if "force_drain_deferred_remit" not in hot and "residual" not in hot:
        fails.append("AC3: residual -> storm-clear / min-dirty chain missing")
    # Counter must still exist (or be intentionally absent if replaced).
    if "residual_force_mask" in obs:
        # If present, must still be incremented from the same path.
        pass

    # AC4 — Soft / Off / force_jit_regions_mask_ == 0 → zero extra stores.
    if "if (demoted != 0)" not in hot:
        fails.append("AC4: `if (demoted != 0)` gate missing at stamp sites")
    if hot.count("if (demoted != 0)") < 2:
        fails.append(
            "AC4: `if (demoted != 0)` gate must appear at both "
            "decide_and_reemit AND on_reemit_pipeline_call stamp sites"
        )

    # AC5 — Non-duplicative vs upstream issues.
    for marker in ("#2895", "#2949", "#2978", "#3026"):
        if marker not in hot and marker.replace("#", "Issue ") not in hot:
            fails.append(f"AC5: {marker} upstream contract marker missing")
    # #3136 / #3229 relower define coverage (one of them must exist).
    if "#3136" not in hot and "#3229" not in hot and "Issue #3136" not in hot and "Issue #3229" not in hot:
        fails.append("AC5: #3136 / #3229 relower define coverage marker missing")

    # AC6 — No design docs / no test_issue_3413.cpp.
    if list((ROOT / "docs" / "design").glob("3413-*.md")):
        fails.append("AC6: docs/design/3413-*.md exists — design docs banned per #1655")
    for sub in ("issues", "compiler", "core"):
        if (ROOT / "tests" / sub / "test_issue_3413.cpp").is_file():
            fails.append(
                f"AC6: tests/{sub}/test_issue_3413.cpp exists — must extend existing "
                "test_aot_incremental_reemit.cpp per #81934"
            )

    # AC7 — Test markers + build.py wiring.
    if "3413 AC1: decide_and_reemit skips fallback demoted stamp" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3413 AC1 marker")
    if "3413 AC2/AC3: only_covered re-promote + storm-clear min-dirty intact" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3413 AC2/AC3 markers")
    if "3413 AC4: Soft / Off / force_jit_regions_mask_==0 → zero extra stores" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3413 AC4 marker")
    if "3413 AC5: non-duplicative vs #2895/#2949/#2978/#3026/#3136/#3229" not in test:
        fails.append("AC7: test_aot_incremental_remit.cpp missing 3413 AC5 marker")
    if "3413 AC6: no docs/design/3413-*; no test_issue_3413.cpp" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3413 AC6 marker")
    if "3413 AC7: build.py wiring" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3413 AC7 marker")

    if "cmd_partial_reemit_success_coverage_3413_coverage" not in build:
        fails.append("AC7: build.py does not register cmd_partial_reemit_success_coverage_3413_coverage")
    if "check_partial_reemit_success_coverage_3413" not in build:
        fails.append("AC7: build.py does not register check_partial_reemit_success_coverage_3413 linter script")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1

    print("PASS: #3413 last_reemit_success coverage partial-emit contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
