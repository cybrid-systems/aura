#!/usr/bin/env python3
# scripts/coverage/checks/check_occurrence_persist_fingerprint_3170.py -- Issue #3170 source-cite gate.
#
# Verifies the P0 residual close-out for type/occurrence — outermost
# success must freeze occurrence persist snapshot only when fingerprint
# matches live goals; abort / nested / force-rollback must clear (I4
# residual from 2026-08 type-system review — 半解不得出厂). Wire-up
# spans four layers:
#
#  1. Counter field (src/compiler/observability_metrics.h, struct end —
#     AC4 additive observability, layout-stable per #2906):
#     - occurrence_persist_fingerprint_mismatch_total: bumped when
#       outermost-success fingerprint guard rejects the staged snapshot
#
#  2. ConstraintSystem helpers (src/compiler/type_checker.ixx decl +
#    src/compiler/type_checker_impl.cpp impl):
#     - clear_occurrence_persist_snapshot() — clears occurrence_persist_log_
#       without touching live occurrence_goals_
#
#  3. TypeChecker wrapper (src/compiler/type_checker.ixx):
#     - clear_occurrence_persist_snapshot() — wraps the CS call +
#       set_metrics(metrics_) for production gate
#
#  4. typed_mutation_audit.h free functions:
#     - occurrence_goal_fingerprint(tc_handle) — FNV-1a hash of live
#       occurrence goals (bounded by kProofGoalFingerprintMaxGoals)
#     - clear_occurrence_persist_buffer(tc_handle) — production-only
#       wrapper that bumps g_occurrence_persist_audit_atomic_wired
#
#  5. evaluator_mutation_boundary.cpp C ABI hooks + wire-up:
#     - aura_clear_occurrence_persist_buffer(ev_ptr) — file-scope per #75724
#     - aura_outermost_success_persist_occurrence — fingerprint guard added
#       before freeze write; mismatch → clear + bump + return early (AC1)
#     - abort / nested / force-rollback paths — call aura_clear_occurrence_persist_buffer
#       before existing return (AC2 uniform enforcement)
#
#  6. Tests: extend tests/compiler/test_occurrence_goal_persist_rehydrate.cpp
#     with ac3170_1..ac3170_6 (extends #2608 / #2641 lineage).
#
#  7. Forbidden artifacts (per #1655 + #81967):
#     - No docs/design/3170-* plan doc
#     - No tests/issues/test_issue_3170.cpp
#     - No tests/compiler/test_issue_3170.cpp
#     - No tests/serve/test_issue_3170.cpp
#     - No new public query key (reuse #2608 / #2641 surfaces)

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
    "src/compiler/typed_mutation_audit.h",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "tests/compiler/test_occurrence_goal_persist_rehydrate.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Counter field at struct end (AC4 additive observability per #2906).
    (
        "src/compiler/observability_metrics.h",
        r"std::atomic<std::uint64_t>\s+occurrence_persist_fingerprint_mismatch_total\{0\}",
        "obs: occurrence_persist_fingerprint_mismatch_total counter",
    ),
    (
        "src/compiler/observability_metrics.h",
        r"//\s*Issue #3170:.*outermost-success Occurrence persist fingerprint guard",
        "obs: counter declaration cites #3170",
    ),
    # ConstraintSystem clear helper decl + impl.
    (
        "src/compiler/type_checker.ixx",
        r"std::size_t\s+clear_occurrence_persist_snapshot\(\)\s+noexcept\s*;",
        "cs: clear_occurrence_persist_snapshot declaration",
    ),
    (
        "src/compiler/type_checker_impl.cpp",
        r"std::size_t\s+ConstraintSystem::clear_occurrence_persist_snapshot\(\)\s+noexcept",
        "cs: clear_occurrence_persist_snapshot impl signature",
    ),
    # TypeChecker wrapper (calls CS clear + set_metrics).
    (
        "src/compiler/type_checker.ixx",
        r"//\s*Issue #3170: outermost-success fingerprint guard \u2014 clear the long-lived",
        "cs: TypeChecker wrapper comment cites #3170",
    ),
    (
        "src/compiler/type_checker.ixx",
        r"return\s+solve_delta_cs_\.clear_occurrence_persist_snapshot\(\);",
        "cs: TypeChecker wrapper delegates to CS clear",
    ),
    # occurrence_goal_fingerprint helper (FNV-1a hash).
    (
        "src/compiler/typed_mutation_audit.h",
        r"\[\[nodiscard\]\]\s+inline\s+std::uint64_t\s+occurrence_goal_fingerprint\(",
        "tma: occurrence_goal_fingerprint helper",
    ),
    (
        "src/compiler/typed_mutation_audit.h",
        r"//\s*Issue #3170:.*occurrence goal fingerprint",
        "tma: fingerprint helper comment cites #3170",
    ),
    # clear_occurrence_persist_buffer wrapper (production gate).
    (
        "src/compiler/typed_mutation_audit.h",
        r"\[\[nodiscard\]\]\s+inline\s+std::uint64_t\s+clear_occurrence_persist_buffer\(",
        "tma: clear_occurrence_persist_buffer wrapper",
    ),
    (
        "src/compiler/typed_mutation_audit.h",
        r"if\s*\(!aura::compiler::typed_audit::production_defaults_active\(\)\)\s*\n\s*return\s+0;",
        "tma: clear wrapper production gate (Soft untouched)",
    ),
    # aura_clear_occurrence_persist_buffer C ABI in evaluator_mutation_boundary.cpp.
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r'extern\s+"C"\s+void\s+aura_clear_occurrence_persist_buffer\(',
        "emb: aura_clear_occurrence_persist_buffer C ABI",
    ),
    # Fingerprint guard in outermost success C ABI hook.
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"//\s*Issue #3170: outermost-success fingerprint guard",
        "emb: fingerprint guard wire-up cite in C ABI hook",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"live_fp\s+!=\s+ev->expected_occurrence_snapshot_fp\(\)",
        "emb: fingerprint match check",
    ),
    # Three abort/nested clear calls (AC2 uniform enforcement).
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"aura_clear_occurrence_persist_buffer\(ev_\);",
        "emb: clear call in outermost abort path (line 2757)",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"//\s*Issue #3170: clear occurrence persist buffer on outermost abort",
        "emb: clear call cite in second abort path (line 4315)",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"aura_clear_occurrence_persist_buffer\(ev_\);[^\\n]*?ev_->note_type_export_inflight\(\);",
        "emb: clear call in nested path (line 4620)",
    ),
    # Test extension: AC1..AC6 calls.
    (
        "tests/compiler/test_occurrence_goal_persist_rehydrate.cpp",
        r"ac3170_1_outermost_success_fingerprint_guard",
        "test: AC1 outermost success fingerprint guard",
    ),
    (
        "tests/compiler/test_occurrence_goal_persist_rehydrate.cpp",
        r"ac3170_2_abort_nested_uniform_clear",
        "test: AC2 abort/nested uniform clear",
    ),
    (
        "tests/compiler/test_occurrence_goal_persist_rehydrate.cpp",
        r"ac3170_3_soft_zero_behavioural_change",
        "test: AC3 soft zero behavioural change",
    ),
    (
        "tests/compiler/test_occurrence_goal_persist_rehydrate.cpp",
        r"ac3170_4_quiet_zero_extra_atomics",
        "test: AC4 quiet zero extra atomics",
    ),
    (
        "tests/compiler/test_occurrence_goal_persist_rehydrate.cpp",
        r"ac3170_5_additive_observability_only",
        "test: AC5 additive observability only",
    ),
    (
        "tests/compiler/test_occurrence_goal_persist_rehydrate.cpp",
        r"ac3170_6_source_and_linter",
        "test: AC6 source/linter suite",
    ),
    # Build.py wiring.
    (
        "build.py",
        r"cmd_occurrence_persist_fingerprint_3170",
        "build: cmd_occurrence_persist_fingerprint_3170 dispatcher",
    ),
    (
        "build.py",
        r"check_occurrence_persist_fingerprint_3170",
        "build: linter path wired",
    ),
)

FORBIDDEN_DOCS: tuple[str, ...] = (
    "docs/design/3170-occurrence-persist-fingerprint.md",
    "docs/design/3170-occurrence-persist-fingerprint-guard.md",
    "docs/design/3170-half-narrowing.md",
)

FORBIDDEN_TESTS: tuple[str, ...] = (
    "tests/issues/test_issue_3170.cpp",
    "tests/compiler/test_issue_3170.cpp",
    "tests/serve/test_issue_3170.cpp",
    "tests/core/test_issue_3170.cpp",
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
        // Issue #3170: outermost-success Occurrence persist fingerprint guard
        std::atomic<std::uint64_t> occurrence_persist_fingerprint_mismatch_total{0};
    };
    // src/compiler/type_checker.ixx
    std::size_t clear_occurrence_persist_snapshot() noexcept;
    // Issue #3170: outermost-success fingerprint guard — clear the long-lived
    // occurrence persist buffer without touching live occurrence_goals_.
    std::size_t clear_occurrence_persist_snapshot() noexcept {
        if (metrics_)
            solve_delta_cs_.set_metrics(metrics_);
        return solve_delta_cs_.clear_occurrence_persist_snapshot();
    }
    // src/compiler/type_checker_impl.cpp
    std::size_t ConstraintSystem::clear_occurrence_persist_snapshot() noexcept {
    // src/compiler/typed_mutation_audit.h
    [[nodiscard]] inline std::uint64_t occurrence_goal_fingerprint(void* tc_handle) {
    // Issue #3170: occurrence goal fingerprint for outermost-success guard.
    [[nodiscard]] inline std::uint64_t clear_occurrence_persist_buffer(void* tc_handle) {
        if (!aura::compiler::typed_audit::production_defaults_active())
            return 0;
    // src/compiler/evaluator_mutation_boundary.cpp
    extern "C" void aura_clear_occurrence_persist_buffer(void* ev_ptr) noexcept {
    // Issue #3170: outermost-success fingerprint guard
    if (aura::compiler::typed_audit::production_defaults_active() &&
        ev->expected_occurrence_snapshot_fp() != 0 &&
        live_fp != ev->expected_occurrence_snapshot_fp()) {
    aura_clear_occurrence_persist_buffer(ev_);
    }
    // Issue #2859
    // Issue #3170: clear occurrence persist buffer on outermost abort
    aura_clear_occurrence_persist_buffer(ev_);
    aura_clear_occurrence_persist_buffer(ev_);
    ev_->note_type_export_inflight();
    // tests/compiler/test_occurrence_goal_persist_rehydrate.cpp
    ac3170_1_outermost_success_fingerprint_guard();
    ac3170_2_abort_nested_uniform_clear();
    ac3170_3_soft_zero_behavioural_change();
    ac3170_4_quiet_zero_extra_atomics();
    ac3170_5_additive_observability_only();
    ac3170_6_source_and_linter();
    // build.py
    cmd_occurrence_persist_fingerprint_3170
    check_occurrence_persist_fingerprint_3170
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
        description="Issue #3170 occurrence persist fingerprint + clear-on-abort/nested source-cite gate",
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
        help="Only check forbidden artifacts (docs/design/3170-*, test_issue_3170.cpp)",
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

    print("OK: Issue #3170 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
