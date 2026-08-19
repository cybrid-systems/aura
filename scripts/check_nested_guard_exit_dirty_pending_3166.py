#!/usr/bin/env python3
# scripts/check_nested_guard_exit_dirty_pending_3166.py -- Issue #3166 source-cite gate.
#
# Verifies the I5 residual for multi-round Agent — nested
# MutationBoundaryGuard exit must force outermost-equivalent dirty +
# restamp authority. The wire-up spans four layers:
#
#  1. Counter fields (src/compiler/observability_metrics.h):
#     - nested_exit_dirty_pending_total (Soft / Off observe, AC2)
#     - nested_exit_dirty_pending_forced_total (Production / Full, AC1)
#
#  2. Nested branch logic (src/compiler/evaluator_mutation_boundary.cpp
#     exit_mutation_boundary, ~L656–690 — nested path that already
#     restamps node_gen):
#     - nested_structural_mutate detection from mutation_log_size delta
#     - production / Full branch: defuse_index_ = nullptr +
#       nested_exit_dirty_pending_forced_total counter bump
#     - Soft / Off branch: nested_exit_dirty_pending_total counter
#       bump only (no behavior change)
#
#  3. Test extension (tests/compiler/test_hygiene_mutate_closed_loop.cpp):
#     - AC1 production + nested structural mutate → post-cascade truth
#       (defuse_index_ invalidated, forced counter bumped)
#     - AC2 Soft / Off → observe counter bumped, no behavior change
#     - AC3 outermost-only path → zero regression (counter not bumped)
#     - AC4 nested abort + outermost → no double restamp / cascade
#
#  4. Forbidden artifacts (per #1655 + #81967):
#     - No docs/design/3166-* plan doc
#     - No tests/issues/test_issue_3166.cpp
#     - No tests/compiler/test_issue_3166.cpp
#     - No new query keys in the middle of metrics surface

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/observability_metrics.h",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "tests/compiler/test_hygiene_mutate_closed_loop.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Layer 1: Counter fields in observability_metrics.h
    (
        "src/compiler/observability_metrics.h",
        r"std::atomic<std::uint64_t>\s+nested_exit_dirty_pending_total\{0\}",
        "nested_exit_dirty_pending_total counter (Soft / Off observe)",
    ),
    (
        "src/compiler/observability_metrics.h",
        r"std::atomic<std::uint64_t>\s+nested_exit_dirty_pending_forced_total\{0\}",
        "nested_exit_dirty_pending_forced_total counter (Production / Full)",
    ),
    (
        "src/compiler/observability_metrics.h",
        r"Issue\s+#3166:\s+nested\s+MutationBoundaryGuard\s+exit\s+dirty\s+pending",
        "Issue #3166 doc comment in observability_metrics.h",
    ),
    # Layer 2: Nested branch logic in evaluator_mutation_boundary.cpp
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"//\s*Issue\s+#3166:\s+I5\s+residual\s+for\s+multi-round\s+Agent",
        "Issue #3166 comment in evaluator_mutation_boundary.cpp nested branch",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"const\s+bool\s+nested_structural_mutate\s*=\s*"
        r"\s*workspace_flat_->mutation_log_size\(\)\s*>\s*cp\.mutation_log_size\s*;",
        "nested_structural_mutate detection (mutation_log_size delta)",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"if\s*\(\s*typed_audit::production_defaults_active\(\)\s*\|\|\s*"
        r"typed_audit::get_strategy\(\)\s*==\s*"
        r"typed_audit::AuditStrategy::Full\s*\)\s*\{[^}]*"
        r"defuse_index_\s*=\s*nullptr\s*;",
        "production/Full branch: defuse_index_ invalidate",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"defuse_index_\s*=\s*nullptr\s*;",
        "defuse_index_ = nullptr inline (AC1 minimal cone invalidate)",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"nested_exit_dirty_pending_forced_total\.fetch_add\(\s*1,\s*"
        r"std::memory_order_relaxed\s*\)",
        "production/Full forced counter bump",
    ),
    (
        "src/compiler/evaluator_mutation_boundary.cpp",
        r"nested_exit_dirty_pending_total\.fetch_add\(\s*1,\s*"
        r"std::memory_order_relaxed\s*\)",
        "Soft/Off observe counter bump",
    ),
    # Layer 3: Test extension
    (
        "tests/compiler/test_hygiene_mutate_closed_loop.cpp",
        r"Issue\s+#3166:\s+nested\s+guard\s+exit\s+dirty\s+pending",
        "Issue #3166 test header comment (matches // or === prefix)",
    ),
    (
        "tests/compiler/test_hygiene_mutate_closed_loop.cpp",
        r"nested_exit_dirty_pending_forced_total|nested_exit_dirty_pending_total",
        "test references #3166 counter",
    ),
    (
        "tests/compiler/test_hygiene_mutate_closed_loop.cpp",
        r"MutationBoundaryGuard\s+(inner|outer)\s*\(",
        "nested MutationBoundaryGuard test pattern",
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
    """Verify no docs/design/3166-* (#1655) or test_issue_3166.cpp (#81967)."""
    failures: list[str] = []
    design_dir = REPO_ROOT / "docs" / "design"
    if design_dir.exists():
        for f in design_dir.iterdir():
            if f.name.startswith("3166-"):
                failures.append(f"docs/design/{f.name}: forbidden per #1655")
    for target_rel in [
        "tests/issues/test_issue_3166.cpp",
        "tests/compiler/test_issue_3166.cpp",
        "tests/serve/test_issue_3166.cpp",
        "tests/core/test_issue_3166.cpp",
        "tests/reflect/test_issue_3166.cpp",
    ]:
        p = REPO_ROOT / target_rel
        if p.exists():
            failures.append(f"{target_rel}: forbidden per #81967")
    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_obs = """
    // Issue #3166: nested MutationBoundaryGuard exit dirty pending
    // (I5 residual for multi-round Agent — closes the window between
    // nested exit and outermost exit under production/Full).
    std::atomic<std::uint64_t> nested_exit_dirty_pending_total{0};
    std::atomic<std::uint64_t> nested_exit_dirty_pending_forced_total{0};
    """
    fixture_evaluator = """
    if (workspace_flat_ && !stack.empty()) {
        workspace_flat_->restamp_all_node_generations();
        // Issue #3166: I5 residual for multi-round Agent — nested exit
        const bool nested_structural_mutate =
            workspace_flat_->mutation_log_size() > cp.mutation_log_size;
        if (nested_structural_mutate) {
            if (typed_audit::production_defaults_active() ||
                typed_audit::get_strategy() == typed_audit::AuditStrategy::Full) {
                defuse_index_ = nullptr;
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                    m->nested_exit_dirty_pending_forced_total.fetch_add(
                        1, std::memory_order_relaxed);
            } else {
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                    m->nested_exit_dirty_pending_total.fetch_add(
                        1, std::memory_order_relaxed);
            }
        }
    }
    """
    fixture_test = """
    // Issue #3166: nested guard exit dirty pending (I5 residual).
    {
        Evaluator::MutationBoundaryGuard outer(ev, &ok);
        {
            Evaluator::MutationBoundaryGuard inner(ev, &ok);
            // nested mutate → nested exit → counter check
            CHECK(m->nested_exit_dirty_pending_total.load() > 0,
                  "Soft / Off observe counter bumped");
            CHECK(m->nested_exit_dirty_pending_forced_total.load() > 0,
                  "Production / Full forced counter bumped");
        }
    }
    """
    fixtures = {
        "src/compiler/observability_metrics.h": fixture_obs,
        "src/compiler/evaluator_mutation_boundary.cpp": fixture_evaluator,
        "tests/compiler/test_hygiene_mutate_closed_loop.cpp": fixture_test,
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
        description="Issue #3166 nested guard exit dirty pending source-cite gate",
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
        help="Only check forbidden artifacts (docs/design/3166-*, test_issue_3166.cpp)",
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

    print("OK: Issue #3166 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
