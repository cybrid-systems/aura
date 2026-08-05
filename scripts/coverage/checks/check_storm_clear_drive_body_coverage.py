#!/usr/bin/env python3
"""check_storm_clear_drive_body_coverage.py — Issue #2669 source gate.

Storm-clear health pass drives recovery body on non-None → None
storm level transition (refine #2639 counter-only baseline). Closes
the residual window where quiet hosts exiting storm via deopt-window
roll without a concurrent pipeline call leave deferred reemit /
force-JIT pending until the next opportunistic pipeline entry.

AC1: Deferred reemit pending → take_deferred_reemit_version() +
     aura_reemit_aot_for_dirty(v) fires on storm-clear edge;
     pending cleared; reemit_driven_total advanced.
AC2: Force-JIT mask pending → aura_hot_update_maybe_retry_exhausted_
     min_dirty() fires (#2601 retry); reemit_driven_total advanced;
     success_total unchanged (success via #2601 counter, no
     double-count per AC4).
AC3: last_region_mask_from_dirty pending → on_cascade_reemit_trigger
     fires (#2502 cascade); reemit_driven_total advanced.
AC4: Quiet path (no pending) → zero extra work; no counter changes.
AC5: Storm re-entry mid-pass → skip + skipped_reentered (existing
     #2639 AC3); deferred not silently dropped (take is exchange-
     not-check, only the skip counter advances under re-entry).
AC6: src-aligned test (extend test_exhausted_min_dirty_reemit)
     + coverage gate (this linter + build.py cmd_storm_clear_drive_
     body_coverage). schema_2669 / issue_2669 sentinels wired in
     snapshot + external API structs.

Default: non-strict (exit 0, prints coverage summary). Use --strict
to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HOT_UPDATE_CPP = ROOT / "src" / "compiler" / "hot_update_registry.cpp"
HOT_UPDATE_HH = ROOT / "src" / "compiler" / "hot_update_registry.hh"
BUILD = ROOT / "build.py"
TEST = ROOT / "tests" / "compiler" / "test_exhausted_min_dirty_reemit.cpp"


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

    hh_text = _read("src/compiler/hot_update_registry.hh")
    cpp_text = _read("src/compiler/hot_update_registry.cpp")

    # AC1: header declares the new reemit_driven atomic + getter.
    must_present(
        HOT_UPDATE_HH,
        "reemit_storm_clear_health_pass_reemit_driven_total_",
        "AC1: header declares reemit_storm_clear_health_pass_reemit_driven_total_ atomic",
    )
    must_present(
        HOT_UPDATE_HH,
        "reemit_storm_clear_health_pass_reemit_driven_total()",
        "AC1: header exposes reemit_storm_clear_health_pass_reemit_driven_total() getter",
    )

    # AC1+AC2+AC3: cpp wires all 3 branches in maybe_storm_clear_health_pass body.
    must_present(
        HOT_UPDATE_CPP,
        "Issue #2669: drive recovery body on storm-clear success path",
        "AC1: cpp cites #2669 drive body comment",
    )
    must_present(
        HOT_UPDATE_CPP,
        "take_deferred_reemit_version",
        "AC1: cpp wires branch 1 deferred reemit take",
    )
    must_present(
        HOT_UPDATE_CPP,
        "aura_reemit_aot_for_dirty",
        "AC1: cpp wires branch 1 aura_reemit_aot_for_dirty drive",
    )
    must_present(
        HOT_UPDATE_CPP,
        "aura_hot_update_maybe_retry_exhausted_min_dirty",
        "AC2: cpp wires branch 2 #2601 retry",
    )
    must_present(
        HOT_UPDATE_CPP,
        "on_cascade_reemit_trigger",
        "AC3: cpp wires branch 3 #2502 cascade",
    )

    # AC4: quiet path zero-cost unchanged (existing #2639 AC2 baseline).
    if cpp_text and "if (prev == StormLevel::None" not in cpp_text:
        failures.append("AC4: cpp missing quiet path condition (prev == StormLevel::None)")
    if cpp_text and "return; // No transition" not in cpp_text:
        failures.append("AC4: cpp missing quiet path early return")

    # AC5: storm re-entry mid-pass → skipped_reentered counter (existing #2639 AC3).
    if cpp_text and "reemit_storm_clear_health_pass_skipped_reentered_storm_total_" not in cpp_text:
        failures.append("AC5: cpp missing skipped_reentered_storm counter bump")

    # AC6: schema-2669 / issue-2669 sentinels wired (Snapshot struct + external API).
    if hh_text:
        for key in (
            "reemit_storm_clear_health_pass_reemit_driven_total",
            "schema_2669",
            "issue_2669",
        ):
            if key not in hh_text:
                failures.append(f"AC6: header missing {key} (Snapshot + external API)")

    # AC6: cpp populate sites (Snapshot fill + extern C bridge hook).
    if cpp_text:
        for needle in (
            "s.reemit_storm_clear_health_pass_reemit_driven_total",
            "out->reemit_storm_clear_health_pass_reemit_driven_total",
        ):
            if needle not in cpp_text:
                failures.append(f"AC6: cpp missing populate site for {needle!r}")

    # AC6: cpp reset site (reset_storm_clear_health_pass_for_test).
    if cpp_text and "reemit_storm_clear_health_pass_reemit_driven_total_.store(0" not in cpp_text:
        failures.append("AC6: cpp missing reset for reemit_driven counter")

    # #2639 / #2601 / #2502 surfaces preserved (additive, not replaced).
    if hh_text and "Issue #2639: storm-clear edge detection" not in hh_text:
        failures.append("AC6: header missing #2639 surface (must be additive, not replaced)")
    if hh_text and "Issue #2601: exhausted min-dirty retry closed loop" not in hh_text:
        failures.append("AC6: header missing #2601 surface (additive)")
    if hh_text and "cascade_reemit_trigger_total_" not in hh_text:
        failures.append("AC6: header missing #2502 cascade counter (additive)")

    # AC6: test file coverage — #2669 ACs present + main() invokes them.
    test_text = _read("tests/compiler/test_exhausted_min_dirty_reemit.cpp")
    for ac_fn in (
        "ac2669_drive_deferred_branch",
        "ac2669_drive_force_jit_branch",
        "ac2669_quiet_path_zero_cost",
        "ac2669_storm_reentry_skips",
        "ac2669_schema_and_source",
    ):
        if ac_fn not in test_text:
            failures.append(f"AC6: test missing {ac_fn} function")
    if "main()" in test_text or "run_test_exhausted_min_dirty_reemit" in test_text:
        for ac_fn in (
            "ac2669_drive_deferred_branch",
            "ac2669_drive_force_jit_branch",
            "ac2669_quiet_path_zero_cost",
            "ac2669_storm_reentry_skips",
            "ac2669_schema_and_source",
        ):
            if f"{ac_fn}()" not in test_text:
                failures.append(f"AC6: main() does not call {ac_fn}()")

    # AC6: build.py wiring.
    build_text = _read("build.py")
    if "check_storm_clear_drive_body_coverage" not in build_text:
        failures.append("AC6: build.py does not reference check_storm_clear_drive_body_coverage linter")
    if "cmd_storm_clear_drive_body_coverage" not in build_text:
        failures.append("AC6: build.py missing cmd_storm_clear_drive_body_coverage function")

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
        "OK: all #2669 ACs satisfied (storm-clear drives recovery body — "
        "3-branch deferred reemit / #2601 retry / #2502 cascade, body-driven "
        "counter, additive to #2639/#2601/#2502, schema-2669/issue-2669 wired)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
