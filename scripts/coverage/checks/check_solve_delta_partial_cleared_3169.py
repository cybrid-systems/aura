#!/usr/bin/env python3
# scripts/coverage/checks/check_solve_delta_partial_cleared_3169.py -- Issue #3169 source-cite gate.
#
# Verifies the P0 residual close-out for type/incremental — production
# solve_delta must clear partial goals / unresolved after TIMEOUT /
# instance-repair failure and hard-reject (half-solution must not leave
# the factory, I3 from 2026-08 type-system review). Wire-up spans four
# layers:
#
#  1. Counter field (src/compiler/observability_metrics.h, struct end —
#     AC4 additive observability, layout-stable per #2906):
#     - solve_delta_partial_cleared_total: bumped when clear helper fires
#
#  2. ConstraintSystem helper (src/compiler/type_checker.ixx decl +
#    src/compiler/type_checker_impl.cpp impl):
#     - clear_partial_goals_and_unresolved() — clears touched_roots_ +
#       pending_full_solve_roots_ + occurrence_priority_roots_ +
#       let_poly_dirty_roots_ + dirty_count_ + production_escalated_ =
#       true; bumps solve_delta_partial_cleared_total on metrics_ when
#       production_defaults_active()
#     - Soft / Off / unit-test default: early-return on
#       !production_defaults_active() — counter never bumps (AC2 invariant)
#
#  3. Production exit wire-up (src/compiler/type_checker_impl.cpp in
#    escalate_if_production, after #3135 lock + #2963 instance-repair +
#    #2277 full-solve):
#     - CONFLICT repair branch (post try_instance_repair_before_full):
#       call clear_partial_goals_and_unresolved() before return CONFLICT
#     - Post-full-solve branch (when full != SOLVED):
#       call clear_partial_goals_and_unresolved() before return
#     - Quiet / happy SOLVED path: zero extra (gated on prior != TIMEOUT
#       early-return + production + non-SOLVED condition, AC3)
#
#  4. Test extension (tests/compiler/test_solve_delta_unresolved_export.cpp
#    extends the #3003 / #2963 / #2913 lineage):
#     - AC1 production + TIMEOUT/CONFLICT → clear + hard reject (source-cite)
#     - AC2 Soft / Off / unit-test default → zero behavioural change
#       (production gate precedes counter bump)
#     - AC3 Quiet (clean / no dirty / no TIMEOUT) → zero extra atomics
#     - AC4 Additive observability only — counter at struct end +
#       existing #2277 / #3003 / #2963 / #2913 surfaces preserved
#     - AC5 Extends existing solve_delta suite — no test_issue_3169.cpp
#       (per #81967); no docs/design/3169-* (per #1655)
#     - AC6 Source-cite + coverage linter + build.py wiring
#
#  5. Forbidden artifacts (per #1655 + #81967):
#     - No docs/design/3169-* plan doc
#     - No tests/issues/test_issue_3169.cpp
#     - No tests/compiler/test_issue_3169.cpp
#     - No tests/serve/test_issue_3169.cpp
#     - No new public query key (reuse #3003 / #2963 / #2913 surfaces)

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/observability_metrics.h",
    "src/compiler/type_checker.ixx",
    "src/compiler/type_checker_impl.cpp",
    "tests/compiler/test_solve_delta_unresolved_export.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Counter field at struct end (AC4 additive observability per #2906).
    (
        "src/compiler/observability_metrics.h",
        r"std::atomic<std::uint64_t>\s+solve_delta_partial_cleared_total\{0\}",
        "obs: solve_delta_partial_cleared_total counter",
    ),
    (
        "src/compiler/observability_metrics.h",
        r"//\s*Issue #3169:.*production solve_delta fail-closed",
        "obs: counter declaration cites #3169",
    ),
    # ConstraintSystem clear helper declaration + impl.
    (
        "src/compiler/type_checker.ixx",
        r"void\s+clear_partial_goals_and_unresolved\(\)\s+noexcept\s*;",
        "cs: clear_partial_goals_and_unresolved declaration",
    ),
    (
        "src/compiler/type_checker.ixx",
        r"//\s*Issue #3169: production fail-closed after TIMEOUT",
        "cs: declaration cites #3169",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"void\s+ConstraintSystem::clear_partial_goals_and_unresolved\(\)\s+noexcept",
        "cs: clear helper impl signature",
    ),
    # Soft / Off early-return gate (AC2 invariant).
    (
        "src/compiler/type_checker_impl.cpp",
        r"if\s*\(\s*!aura::compiler::typed_audit::production_defaults_active\(\)\s*\)\s*\n\s*return\s*;",
        "cs: clear helper production gate (Soft early-return)",
    ),
    # Counter only bumps under production gate.
    (
        "src/compiler/type_checker_impl.cpp",
        r"solve_delta_partial_cleared_total\.fetch_add\(\s*1,\s*std::memory_order_relaxed\s*\)",
        "cs: solve_delta_partial_cleared_total bump",
    ),
    # Clear set: touched_roots_ + pending_full_solve_roots_ + occurrence +
    # let_poly_dirty + dirty_count_ + production_escalated_ = true.
    (
        "src/compiler/type_checker_impl.cpp",
        r"touched_roots_\.clear\(\)",
        "cs: clear helper clears touched_roots_",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"pending_full_solve_roots_\.clear\(\)",
        "cs: clear helper clears pending_full_solve_roots_",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"occurrence_priority_roots_\.clear\(\)",
        "cs: clear helper clears occurrence_priority_roots_",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"let_poly_dirty_roots_\.clear\(\)",
        "cs: clear helper clears let_poly_dirty_roots_",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"production_escalated_\s*=\s*true",
        "cs: clear helper sets production_escalated_",
    ),
    # Wire-up: clear helper called from CONFLICT repair branch + post-full-solve
    # branch in escalate_if_production.
    (
        "src/compiler/type_checker_impl.cpp",
        r"//\s*Issue #3169: clear any partial goal / unresolved state before",
        "cs: wire-up cite at production exit paths",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"clear_partial_goals_and_unresolved\(\)\s*;\s*\n\s*return\s+SolveResult::CONFLICT",
        "cs: CONFLICT repair branch calls clear helper",
    ),
    # Existing escalation pipeline preserved (#2277 / #3003 / #2963 / #2913).
    (
        "src/compiler/type_checker_impl.cpp",
        r"delta_timeout_full_solve_total",
        "cs: #2277 escalation counter preserved",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"delta_timeout_reject_total",
        "cs: #3003 reject counter preserved",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"try_instance_repair_before_full",
        "cs: #2963 instance repair preserved",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"escalate_locality_slo_if_production",
        "cs: #2913 locality gate preserved",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"if\s*\(\s*prior\s*!=\s*SolveResult::TIMEOUT\s*\)\s*\n\s*return\s+prior\s*;",
        "cs: early-return on non-TIMEOUT (AC3 quiet path)",
    ),
    # Test extension: AC1..AC6.
    (
        "tests/compiler/test_solve_delta_unresolved_export.cpp",
        r"ac3169_1_production_clear_partial_and_reject",
        "test: AC1 production clear + reject",
    ),
    (
        "tests/compiler/test_solve_delta_unresolved_export.cpp",
        r"ac3169_2_soft_zero_extra",
        "test: AC2 soft zero extra",
    ),
    (
        "tests/compiler/test_solve_delta_unresolved_export.cpp",
        r"ac3169_3_quiet_zero_extra",
        "test: AC3 quiet zero extra",
    ),
    (
        "tests/compiler/test_solve_delta_unresolved_export.cpp",
        r"ac3169_4_additive_counter_only",
        "test: AC4 additive counter only",
    ),
    (
        "tests/compiler/test_solve_delta_unresolved_export.cpp",
        r"ac3169_5_existing_3003_2963_2913_preserved",
        "test: AC5 existing 3003/2963/2913 preserved",
    ),
    (
        "tests/compiler/test_solve_delta_unresolved_export.cpp",
        r"ac3169_6_source_and_linter",
        "test: AC6 source/linter suite",
    ),
    # Build.py wiring.
    (
        "build.py",
        r"cmd_solve_delta_partial_cleared_3169",
        "build: cmd_solve_delta_partial_cleared_3169 dispatcher",
    ),
    (
        "build.py",
        r"check_solve_delta_partial_cleared_3169",
        "build: linter path wired",
    ),
)

FORBIDDEN_DOCS: tuple[str, ...] = (
    "docs/design/3169-solve-delta-partial-cleared.md",
    "docs/design/3169-production-fail-closed.md",
    "docs/design/3169-half-solution.md",
)

FORBIDDEN_TESTS: tuple[str, ...] = (
    "tests/issues/test_issue_3169.cpp",
    "tests/compiler/test_issue_3169.cpp",
    "tests/serve/test_issue_3169.cpp",
    "tests/core/test_issue_3169.cpp",
)


def check_file(rel: str, rx: str, strict: bool = True) -> list[str]:
    p = REPO_ROOT / rel
    if not p.exists():
        return [f"MISSING: {rel}"]
    text = p.read_text(encoding="utf-8", errors="replace")
    if re.search(rx, text, re.MULTILINE | re.DOTALL):
        return []
    if strict:
        return [f"MISSING PATTERN: {rel} :: {rx}"]
    return []


def check_no_forbidden_artifacts() -> list[str]:
    failures: list[str] = []
    for rel in FORBIDDEN_DOCS + FORBIDDEN_TESTS:
        p = REPO_ROOT / rel
        if p.exists():
            failures.append(f"FORBIDDEN ARTIFACT: {rel}")
    return failures


def _self_test() -> int:
    fixture = """
    // src/compiler/observability_metrics.h
    struct CompilerMetrics {
        // ... existing counters ...
        // Issue #3169: production solve_delta fail-closed
        std::atomic<std::uint64_t> solve_delta_partial_cleared_total{0};
    };
    // src/compiler/type_checker.ixx
    // Issue #3169: production fail-closed after TIMEOUT
    void clear_partial_goals_and_unresolved() noexcept;
    // src/compiler/type_checker_impl.cpp
    void ConstraintSystem::clear_partial_goals_and_unresolved() noexcept {
        if (!aura::compiler::typed_audit::production_defaults_active())
            return;
        touched_roots_.clear();
        pending_full_solve_roots_.clear();
        occurrence_priority_roots_.clear();
        let_poly_dirty_roots_.clear();
        dirty_count_ = 0;
        production_escalated_ = true;
        static_cast<struct CompilerMetrics*>(metrics_)
            ->solve_delta_partial_cleared_total.fetch_add(1, std::memory_order_relaxed);
    }
    SolveResult ConstraintSystem::escalate_if_production(SolveResult prior, ...) {
        if (prior != SolveResult::TIMEOUT)
            return prior;
        delta_timeout_full_solve_total;
        delta_timeout_reject_total;
        try_instance_repair_before_full;
        escalate_locality_slo_if_production;
        // Issue #3169: clear any partial goal / unresolved state before
        clear_partial_goals_and_unresolved();
        return SolveResult::CONFLICT;
    }
    // tests/compiler/test_solve_delta_unresolved_export.cpp
    ac3169_1_production_clear_partial_and_reject();
    ac3169_2_soft_zero_extra();
    ac3169_3_quiet_zero_extra();
    ac3169_4_additive_counter_only();
    ac3169_5_existing_3003_2963_2913_preserved();
    ac3169_6_source_and_linter();
    // build.py
    cmd_solve_delta_partial_cleared_3169
    check_solve_delta_partial_cleared_3169
    """
    failures: list[str] = []
    for rel, rx, _label in INFRA_REQUIRED:
        if not re.search(rx, fixture, re.MULTILINE | re.DOTALL):
            failures.append(f"SELF-TEST: pattern missing for {rel} :: {rx}")
    if failures:
        print("SELF-TEST FAIL:")
        for line in failures:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns found in fixture")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3169 production solve_delta fail-closed + clear partial source-cite gate",
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
        help="Only check forbidden artifacts (docs/design/3169-*, test_issue_3169.cpp)",
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

    print("OK: Issue #3169 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
