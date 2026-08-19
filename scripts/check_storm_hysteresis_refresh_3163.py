#!/usr/bin/env python3
# scripts/check_storm_hysteresis_refresh_3163.py -- Issue #3163 source-cite gate.
#
# Verifies the storm-exit hysteresis refresh on None→non-None edge is wired
# across the two layers:
#
#  1. Producer hook (src/compiler/hot_update_registry.cpp
#     storm_exit_force_full_active):
#     - Existing storm→None edge still arms kStormExitForceFullConsults
#     - New None→non-None edge refreshes the counter so alternating
#       Shape↔Global storms get a full hysteresis on each exit
#     - storm_exit_force_full_remaining_ reset on entry edge prevents
#       short-oscillation partial↔full on consecutive mutates
#
#  2. Test extension (tests/compiler/test_partial_relower_storm_gate.cpp):
#     - AC6 alternating-storm soak: Shape→exit→Global→exit cycle
#     - Verifies forced_full_total advances under alternation
#     - Verifies consult_total advances by the expected pattern
#     - Source-cite: refresh-on-entry cites Issue #3163
#
#  3. Forbidden artifacts (per #1655 + #81967):
#     - No docs/design/3163-* plan doc
#     - No tests/issues/test_issue_3163.cpp
#     - No new middle-of-metrics keys (existing counters reused)

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/hot_update_registry.cpp",
    "tests/compiler/test_partial_relower_storm_gate.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Layer 1: storm_exit_force_full_active refresh on None→non-None edge
    (
        "src/compiler/hot_update_registry.cpp",
        r"if\s*\(\s*now\s*==\s*0\s*&&\s*prev\s*!=\s*0\s*\)",
        "storm→None edge condition (existing arm)",
    ),
    (
        "src/compiler/hot_update_registry.cpp",
        r"else\s+if\s*\(\s*now\s*!=\s*0\s*&&\s*prev\s*==\s*0\s*\)",
        "None→non-None edge condition (#3163 refresh)",
    ),
    (
        "src/compiler/hot_update_registry.cpp",
        r"//\s*Issue\s+#3163:\s*storm entry edge",
        "Issue #3163 comment in storm_exit_force_full_active",
    ),
    (
        "src/compiler/hot_update_registry.cpp",
        r"kStormExitForceFullConsults",
        "kStormExitForceFullConsults constant present (existing + #3163)",
    ),
    (
        "src/compiler/hot_update_registry.cpp",
        r"storm_exit_force_full_remaining_\.store\(\s*kStormExitForceFullConsults",
        "counter.store(kStormExitForceFullConsults, ...) present",
    ),
    # Layer 2: test extension AC6 alternating-storm soak
    (
        "tests/compiler/test_partial_relower_storm_gate.cpp",
        r"AC6\s*\(Issue\s+#3163\):\s*alternating",
        "AC6 test header (#3163)",
    ),
    (
        "tests/compiler/test_partial_relower_storm_gate.cpp",
        r"aura_hot_update_set_shape_storm_active\(1\)",
        "AC6 Shape-storm set call",
    ),
    (
        "tests/compiler/test_partial_relower_storm_gate.cpp",
        r"trip_global_storm\(\)",
        "AC6 Global-storm trip call",
    ),
    (
        "tests/compiler/test_partial_relower_storm_gate.cpp",
        r"apply_partial_relower_storm_gate\(true\)",
        "AC6 post-clear storm gate apply",
    ),
    (
        "tests/compiler/test_partial_relower_storm_gate.cpp",
        r"AC6:\s*forced_full advances under alternating storms",
        "AC6 forced_full check label",
    ),
    (
        "tests/compiler/test_partial_relower_storm_gate.cpp",
        r"AC6:\s*consult_total advances by alternating storm pattern",
        "AC6 consult_total check label",
    ),
    (
        "tests/compiler/test_partial_relower_storm_gate.cpp",
        r"AC6:\s*refresh-on-entry cites Issue\s+#3163",
        "AC6 source-cite label",
    ),
    (
        "tests/compiler/test_partial_relower_storm_gate.cpp",
        r"AC6:\s*None.*non-None edge refresh wired",
        "AC6 None→non-None edge check label",
    ),
)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def check_file(rel_path: str, regex: str, *, strict: bool) -> list[str]:
    failures: list[str] = []
    p = REPO_ROOT / rel_path
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    if not re.search(regex, text) and strict:
        failures.append(f"{rel_path}: missing required pattern: {regex!r}")
    return failures


def check_no_forbidden_artifacts() -> list[str]:
    """Verify no docs/design/3163-* (#1655) or test_issue_3163.cpp (#81967)."""
    failures: list[str] = []
    design_dir = REPO_ROOT / "docs" / "design"
    if design_dir.exists():
        for f in design_dir.iterdir():
            if f.name.startswith("3163-"):
                failures.append(f"docs/design/{f.name}: forbidden per #1655")
    issues_dir = REPO_ROOT / "tests" / "issues"
    if issues_dir.exists():
        target = issues_dir / "test_issue_3163.cpp"
        if target.exists():
            failures.append("tests/issues/test_issue_3163.cpp: forbidden per #81967")
    compiler_issues = REPO_ROOT / "tests" / "compiler" / "test_issue_3163.cpp"
    if compiler_issues.exists():
        failures.append("tests/compiler/test_issue_3163.cpp: forbidden per #81967")
    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_hur = """
    bool HotUpdateRegistry::storm_exit_force_full_active() noexcept {
        const auto now = static_cast<std::uint8_t>(current_storm_level());
        const auto prev = hysteresis_prev_storm_level_.exchange(now, std::memory_order_acq_rel);
        if (now == 0 && prev != 0) {
            storm_exit_force_full_remaining_.store(kStormExitForceFullConsults,
                                                   std::memory_order_release);
        } else if (now != 0 && prev == 0) {
            // Issue #3163: storm entry edge (None -> non-None) refreshes the
            // hysteresis window so alternating Shape↔Global storms get a full
            // hysteresis on each exit.
            storm_exit_force_full_remaining_.store(kStormExitForceFullConsults,
                                                   std::memory_order_release);
        }
        if (now != 0)
            return false;
        auto left = storm_exit_force_full_remaining_.load(std::memory_order_acquire);
        while (left > 0) {
            if (storm_exit_force_full_remaining_.compare_exchange_weak(
                    left, left - 1, std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
        return false;
    }
    """
    fixture_test = """
    // ── AC6 (Issue #3163): alternating Shape↔Global storm hysteresis ──
    {
        aura_hot_update_set_shape_storm_active(1);
        trip_global_storm();
        apply_partial_relower_storm_gate(true);
        CHECK(f1 > f0, "AC6: forced_full advances under alternating storms");
        CHECK(c1 >= c0 + static_cast<std::uint64_t>(kAltIters * 3),
              "AC6: consult_total advances by alternating storm pattern");
        CHECK(fn_win.find("Issue #3163") != std::string::npos,
              "AC6: refresh-on-entry cites Issue #3163");
        CHECK(fn_win.find("now != 0 && prev == 0") != std::string::npos,
              "AC6: None->non-None edge refresh wired");
    }
    """
    fixtures = {
        "src/compiler/hot_update_registry.cpp": fixture_hur,
        "tests/compiler/test_partial_relower_storm_gate.cpp": fixture_test,
    }
    fails: list[str] = []
    for rel, rx, label in INFRA_REQUIRED:
        text = fixtures.get(rel, "")
        if not re.search(rx, text):
            fails.append(f"{rel}: missing required pattern: {rx!r} (label: {label})")
    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns found in fixture")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3163 storm-exit hysteresis refresh source-cite gate",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=True,
        help="Fail on missing patterns (default: strict)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Validate linter regex / structure against fixture text",
    )
    parser.add_argument(
        "--forbidden-only",
        action="store_true",
        help="Only check forbidden artifacts (docs/design/3163-*, test_issue_3163.cpp)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures: list[str] = []

    if not args.forbidden_only:
        for rel, rx, _label in INFRA_REQUIRED:
            failures.extend(check_file(rel, rx, strict=args.strict))

    failures.extend(check_no_forbidden_artifacts())

    if failures:
        print(f"FAIL: {len(failures)} issue(s):")
        for line in failures:
            print(f"  {line}")
        return 1

    print("OK: Issue #3163 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
