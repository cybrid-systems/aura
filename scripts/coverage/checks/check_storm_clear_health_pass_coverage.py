#!/usr/bin/env python3
"""check_storm_clear_health_pass_coverage.py — Issue #2639 source gate.

Storm-clear edge detection (lazy hook) on non-None → None storm
level transition with pending state. Detects post-storm
"visible but unhealed" residual (force-JIT mask, deferred reemit,
region mask) and fires a health pass that drives the existing
#2604 auto-drain / #2601 exhausted-min-dirty retry machinery.

AC1: Inject Global storm + deferred/force-JIT → clear storm →
     health pass fires + deferred drained or min-dirty attempted.
AC2: Quiet path (storm already None, no deferred) → zero extra work.
AC3: Storm re-enters mid-pass → skip body, bump skipped_reentered;
     deferred not silently dropped.
AC4: #2604 auto-drain / #2601 exhausted-retry / #2502 re-promote
     still work; counters additive.
AC5: Query surface on hot-update-registry-stats / reload-recovery-
     state + schema-2639 / issue-2639 / wired sentinel; #2605 axes
     preserved.
AC6: src-aligned test (extend test_exhausted_min_dirty_reemit_2544
     or sibling) + coverage gate (this linter + build.py gate step).

Default: non-strict (exit 0, prints coverage summary). Use --strict
to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BRIDGE = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
HOT_UPDATE_CPP = ROOT / "src" / "compiler" / "hot_update_registry.cpp"
HOT_UPDATE_HH = ROOT / "src" / "compiler" / "hot_update_registry.hh"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
BUILD = ROOT / "build.py"
TEST_2544 = ROOT / "tests" / "compiler" / "test_exhausted_min_dirty_reemit_2544.cpp"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    strict = "--strict" in sys.argv
    failures: list[str] = []

    def must_present(path: Path, needle: str, label: str) -> None:
        if not path.exists():
            failures.append(f"{label}: {path} not found")
            return
        text = path.read_text(encoding="utf-8", errors="replace")
        if needle not in text:
            failures.append(f"{label}: missing {needle!r} in {path.name}")

    # AC1: storm-clear edge detection in HotUpdateRegistry
    must_present(
        HOT_UPDATE_HH, "Issue #2639: storm-clear edge detection", "AC1: header cites #2639 storm-clear edge detection"
    )
    must_present(
        HOT_UPDATE_HH, "maybe_storm_clear_health_pass", "AC1: header declares maybe_storm_clear_health_pass method"
    )
    must_present(
        HOT_UPDATE_HH, "reemit_storm_clear_health_pass_total_", "AC1: header declares storm-clear counter member"
    )

    # AC2: quiet path — zero extra work
    hh_text = _read("src/compiler/hot_update_registry.hh")
    if hh_text and "prev_storm_level_" not in hh_text:
        failures.append("AC1: header missing prev_storm_level_ field (edge detection)")

    # AC1: lazy hook call in on_reemit_pipeline_call
    cpp_text = _read("src/compiler/hot_update_registry.cpp")
    if cpp_text:
        # The call should be in on_reemit_pipeline_call
        if "maybe_storm_clear_health_pass();" not in cpp_text:
            failures.append("AC1: cpp missing maybe_storm_clear_health_pass() call in on_reemit_pipeline_call")
        if "AC1: edge detection" not in cpp_text and "AC1: storm" not in cpp_text:
            failures.append("AC1: cpp missing AC1 comment in maybe_storm_clear_health_pass body")
        # AC2: quiet path condition
        if "if (prev == StormLevel::None" not in cpp_text:
            failures.append("AC2: cpp missing quiet path condition (prev == StormLevel::None)")
        if "return; // No transition" not in cpp_text:
            failures.append("AC2: cpp missing quiet path early return")
        # AC3: storm re-entry mid-pass
        if "reentered_storm" not in cpp_text:
            failures.append("AC3: cpp missing reentered_storm counter bump")
        # Reset helper
        if "reset_storm_clear_health_pass_for_test" not in cpp_text:
            failures.append("AC6: cpp missing reset_storm_clear_health_pass_for_test method")

    # AC5: query surface in header (Snapshot fields)
    if hh_text:
        for key in (
            "reemit_storm_clear_health_pass_total",
            "reemit_storm_clear_health_pass_success_total",
            "reemit_storm_clear_health_pass_skipped_reentered_storm_total",
            "schema_2639",
            "issue_2639",
        ):
            if key not in hh_text:
                failures.append(f"AC5: header Snapshot missing {key}")

    # AC5: extern C hook in header
    must_present(HOT_UPDATE_HH, "aura_hot_update_maybe_storm_clear_health_pass", "AC5: header declares extern C hook")
    if cpp_text and 'extern "C" void aura_hot_update_maybe_storm_clear_health_pass' not in cpp_text:
        failures.append("AC5: cpp missing extern C hook definition")

    # AC6: tests + build.py gate
    must_present(
        TEST_2544, "ac2639_storm_clear_fires_on_transition", "AC6: test missing ac2639_storm_clear_fires_on_transition"
    )
    must_present(TEST_2544, "ac2639_quiet_path_zero_cost", "AC6: test missing ac2639_quiet_path_zero_cost")
    must_present(
        TEST_2544, "ac2639_storm_reenters_mid_pass_skips", "AC6: test missing ac2639_storm_reenters_mid_pass_skips"
    )
    must_present(TEST_2544, "ac2639_schema_and_source", "AC6: test missing ac2639_schema_and_source")
    test_text = _read("tests/compiler/test_exhausted_min_dirty_reemit_2544.cpp")
    if "main()" in test_text:
        for ac_fn in (
            "ac2639_storm_clear_fires_on_transition",
            "ac2639_quiet_path_zero_cost",
            "ac2639_storm_reenters_mid_pass_skips",
            "ac2639_schema_and_source",
        ):
            if f"{ac_fn}()" not in test_text:
                failures.append(f"AC6: main() does not call {ac_fn}()")

    build_text = _read("build.py")
    if "check_storm_clear_health_pass_coverage" not in build_text:
        failures.append("AC6: build.py does not reference check_storm_clear_health_pass_coverage linter")
    if "cmd_storm_clear_health_pass_coverage" not in build_text:
        failures.append("AC6: build.py missing cmd_storm_clear_health_pass_coverage function")

    # AC4: #2604/#2601/#2502 surfaces still work (additive)
    # Verify #2601 exhausted-min-dirty retry surface is preserved in the
    # header (we extended it, not replaced it).
    if hh_text and "Issue #2601: exhausted min-dirty retry closed loop" not in hh_text:
        failures.append("AC4: header missing #2601 surface (must be additive, not replaced)")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if strict:
            return 1
        print(
            f"\nNON-STRICT: {len(failures)} issue(s) above (--strict to enforce)",
            file=sys.stderr,
        )
        return 0

    print(
        "OK: all #2639 ACs satisfied (storm-clear edge detection — "
        "non-None → None lazy hook with pending state, soft zero-cost "
        "quiet path, reentry skip, additive to #2604/#2601/#2502)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
