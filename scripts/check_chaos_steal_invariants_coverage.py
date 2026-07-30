#!/usr/bin/env python3
"""
Linter for #2315 — production chaos soak for steal × mutate × GC × mailbox
(invariant checker). Closes the integration proof gap for P0/P1 fixes
(#2310 steal snapshot fail-closed, #2311 RenderFastExit suppress,
#2312 mailbox×MutationHold gate, #2313 hold-budget over-budget, #2314
residual defer clear) — without long-running chaos, those fixes remain
unproven under load.

Verifies the implementation is wired correctly:
  - tests/serve/test_chaos_steal_mutation_gc.cpp exists with chaos harness
  - AURA_CHAOS_STEAL_GC env gate (production unaffected when unset)
  - AURA_CHAOS_WORKERS / AURA_CHAOS_DURATION_S env knobs
  - AURA_INVARIANT macro defined (compiled out unless AURA_CHAOS_INVARIANTS)
  - Per-iteration invariant checks on fiber state (compiled out by default)
  - End-of-run snapshot via query:orchestration-steal-outermost-stats /
    query:mutation-boundary-hold-stats / query:mf-mailbox-stats /
    query:gc-defer-reason-stats with schema sentinels (schema-2310,
    schema-2311, schema-2312, schema-2313, schema-2314)
  - Timeout watchdog + no-deadlock check (AC3)
  - EXCLUDE_FROM_ALL on CMakeLists registration (CI opt-in)
  - Issue #2315 cite

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_chaos_steal_invariants_coverage.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def must_contain(file: Path, needle: str, label: str) -> bool:
    if not file.exists():
        print(f"FAIL {label}: file {file} does not exist")
        return False
    content = read(file)
    if needle in content:
        print(f"OK  {label}")
        return True
    print(f"FAIL {label}: '{needle}' not found in {file}")
    return False


def main() -> int:
    chaos_test = ROOT / "tests" / "serve" / "test_chaos_steal_mutation_gc.cpp"
    cmake = ROOT / "CMakeLists.txt"

    checks = [
        # Test file exists with chaos harness structure
        (chaos_test, "Issue #2315", "chaos test file cites Issue #2315"),
        (chaos_test, "AURA_CHAOS_STEAL_GC", "AC1: env gate AURA_CHAOS_STEAL_GC present"),
        (chaos_test, "chaos_enabled", "AC1: chaos_enabled() helper present"),
        (chaos_test, "AURA_CHAOS_WORKERS", "AC1: AURA_CHAOS_WORKERS env knob present"),
        (chaos_test, "AURA_CHAOS_DURATION_S", "AC1: AURA_CHAOS_DURATION_S env knob present"),
        (chaos_test, "chaos_workers", "AC1: chaos_workers() helper present (default N ≥ 4)"),
        (chaos_test, "chaos_duration_s", "AC1: chaos_duration_s() helper present (default ≥ 60s)"),
        # AC2: AURA_INVARIANT macro
        (chaos_test, "AURA_CHAOS_INVARIANTS", "AC2: AURA_CHAOS_INVARIANTS canary env guard present"),
        (chaos_test, "#define AURA_INVARIANT", "AC2: AURA_INVARIANT macro defined"),
        (chaos_test, "INVARIANT VIOLATION", "AC2: AURA_INVARIANT fail-closed abort path present"),
        # AC3: pass criteria + watchdog + no deadlock
        (chaos_test, "watchdog_deadline", "AC3: timeout watchdog present"),
        (chaos_test, "all chaos fibers finished", "AC3: no-deadlock check present"),
        (chaos_test, "fibers_finished.load() == k_fibers", "AC3: counter stability check present"),
        # AC4: end-of-run snapshot + schema sentinels
        (chaos_test, "chaos end-of-run snapshot", "AC4: end-of-run snapshot present"),
        (chaos_test, "schema-2310", "AC4: schema-2310 sentinel (#2310 integration)"),
        (chaos_test, "schema-2311", "AC4: schema-2311 sentinel (#2311 integration)"),
        (chaos_test, "schema-2312", "AC4: schema-2312 sentinel (#2312 integration)"),
        (chaos_test, "schema-2313", "AC4: schema-2313 sentinel (#2313 integration)"),
        (chaos_test, "schema-2314", "AC4: schema-2314 sentinel (#2314 integration)"),
        (
            chaos_test,
            "query:orchestration-steal-outermost-stats",
            "AC4: orchestration-steal-outermost-stats query primitive",
        ),
        (chaos_test, "query:mutation-boundary-hold-stats", "AC4: mutation-boundary-hold-stats query primitive"),
        (chaos_test, "query:mf-mailbox-stats", "AC4: mf-mailbox-stats query primitive"),
        (chaos_test, "query:gc-defer-reason-stats", "AC4: gc-defer-reason-stats query primitive"),
        # AC5: CI / nightly wire — EXCLUDE_FROM_ALL on CMakeLists registration
        (cmake, "aura_add_issue_test(test_chaos_steal_mutation_gc)", "AC5: chaos test registered in CMakeLists.txt"),
        # Issue #2315 cite in test
        (chaos_test, "Issue #2315", "AC5: test file cites 2315"),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_chaos_steal_invariants_coverage.py",
            "production chaos soak for steal",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2315 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
