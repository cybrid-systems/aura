#!/usr/bin/env python3
# scripts/check_pcv_span_stale_coverage_3167.py -- Issue #3167 source-cite gate.
#
# Verifies the I2 residual for multi-round Agent — SafePCVSpan /
# children_safe_view must not remain live across MutationBoundaryGuard
# without pin or forced re-query. Production must treat as stale or
# force refresh. Wire-up spans four layers:
#
#  1. PCV header (src/core/persistent_child_vector.hh):
#     - 6-arg SafePCVSpan ctor capturing (node_id, generation,
#       wrap_epoch, node_gen) at children_safe_view time
#     - is_stale(current_generation, current_wrap_epoch, current_node_gen)
#     - captured_node_id_/captured_generation_/captured_wrap_epoch_/
#       captured_node_gen_ accessors + has_fingerprint()
#     - fingerprint shape static_assert
#
#  2. FlatAST (src/core/ast.ixx):
#     - children_safe_view passes FlatAST generation_/wrap_epoch_/node_gen_
#       to the SafePCVSpan 6-arg ctor
#     - force_refresh_pcv_span(safe, id): stale → bump counter +
#       re-pin via children_safe_view; fresh → return original span
#       (AC2 happy path); legacy / no-owner → short-circuit
#     - pcv_span_stale_across_guard_total() accessor (additive metric,
#       AC4 — does not replace #2906 keys)
#
#  3. Test extension (tests/core/test_pcv_exclusive_with_set.cpp +
#     tests/core/test_pcv_workspace_batch.cpp +
#     tests/compiler/test_hygiene_mutate_closed_loop.cpp):
#     - AC1 production stale/refresh across Guard
#     - AC2 happy path zero extra (counter unchanged)
#     - AC3 #2906 non-regression — flatast-locked-move-out-exclusive-total
#       still surfaces, schema unchanged
#     - AC4 additive counter only — pcv_pin_count + pcv_columnar_hit_rate_bp
#       intact
#     - AC5 extend existing suite — no test_issue_3167.cpp (per #81967)
#     - AC6 linter + no docs/design/3167-* (per #1655)
#
#  4. Forbidden artifacts (per #1655 + #81967):
#     - No docs/design/3167-* plan doc
#     - No tests/issues/test_issue_3167.cpp
#     - No tests/compiler/test_issue_3167.cpp
#     - No tests/serve/test_issue_3167.cpp
#     - No new query keys that replace #2906 surface

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/core/persistent_child_vector.hh",
    "src/core/ast.ixx",
    "tests/core/test_pcv_exclusive_with_set.cpp",
    "tests/core/test_pcv_workspace_batch.cpp",
    "tests/compiler/test_hygiene_mutate_closed_loop.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # PCV header: 6-arg ctor + is_stale + fingerprint accessors + static_assert
    (
        "src/core/persistent_child_vector.hh",
        r"Issue #3167:.*6-arg ctor captures FlatAST",
        "pcv: 6-arg ctor header comment",
    ),
    (
        "src/core/persistent_child_vector.hh",
        r"SafePCVSpan\(\s*std::span<const T>\s+sp,\s+"
        r"std::shared_ptr<const[\s\S]+?>\s+keep,\s+"
        r"std::uint32_t\s+node_id,\s+"
        r"std::uint64_t\s+generation,\s+"
        r"std::uint32_t\s+wrap_epoch,\s+"
        r"std::uint16_t\s+node_gen\)\s*(noexcept)?",
        "pcv: 6-arg SafePCVSpan ctor signature",
    ),
    (
        "src/core/persistent_child_vector.hh",
        r"\bis_stale\(\s*std::uint64_t\s+current_generation",
        "pcv: is_stale(current_generation, ...)",
    ),
    (
        "src/core/persistent_child_vector.hh",
        r"captured_node_id\(\)|captured_generation\(\)|captured_wrap_epoch\(\)|captured_node_gen\(\)",
        "pcv: fingerprint accessors",
    ),
    (
        "src/core/persistent_child_vector.hh",
        r"has_fingerprint\(\)",
        "pcv: has_fingerprint() accessor",
    ),
    (
        "src/core/persistent_child_vector.hh",
        r"kPcvSpanNoOwner\s*=\s*(UINT32_MAX|std::numeric_limits<std::uint32_t>::max\(\))",
        "pcv: kPcvSpanNoOwner sentinel",
    ),
    (
        "src/core/persistent_child_vector.hh",
        r"safe_pcv_fingerprint_shape",
        "pcv: fingerprint shape static_assert",
    ),
    # FlatAST: 6-arg capture + force_refresh + counter accessor
    (
        "src/core/ast.ixx",
        r"force_refresh_pcv_span",
        "ast: force_refresh_pcv_span member",
    ),
    (
        "src/core/ast.ixx",
        r"pcv_span_stale_across_guard_total",
        "ast: counter accessor / bump site",
    ),
    (
        "src/core/ast.ixx",
        r"Issue #3167: capture FlatAST node_id \+ generation_",
        "ast: children_safe_view capture comment",
    ),
    # Test extension: AC1 production stale/refresh + AC2 happy path
    (
        "tests/core/test_pcv_exclusive_with_set.cpp",
        r"ac3167_1_production_stale_refresh",
        "test: AC1 production stale refresh",
    ),
    (
        "tests/core/test_pcv_exclusive_with_set.cpp",
        r"ac3167_2_happy_path_zero_extra",
        "test: AC2 happy path zero extra",
    ),
    (
        "tests/core/test_pcv_exclusive_with_set.cpp",
        r"ac3167_4_additive_counter_only",
        "test: AC4 additive counter only",
    ),
    # Test extension: AC3 #2906 non-regression + AC5/AC6 source/lint
    (
        "tests/compiler/test_hygiene_mutate_closed_loop.cpp",
        r"ac3167_3_2906_non_regression",
        "test: AC3 #2906 non-regression",
    ),
    (
        "tests/compiler/test_hygiene_mutate_closed_loop.cpp",
        r"ac3167_6_source_and_linter",
        "test: AC6 source/lint suite",
    ),
    # Batch smoke check (delegates to run_test_pcv_exclusive_with_set)
    (
        "tests/core/test_pcv_workspace_batch.cpp",
        r"Issue #3167: SafePCVSpan stale-across-guard batch smoke check",
        "batch: #3167 smoke check comment",
    ),
)

FORBIDDEN_DOCS: tuple[str, ...] = (
    "docs/design/3167-pcv-span-stale.md",
    "docs/design/3167-pcv-span-stale-across-guard.md",
)

FORBIDDEN_TESTS: tuple[str, ...] = (
    "tests/issues/test_issue_3167.cpp",
    "tests/compiler/test_issue_3167.cpp",
    "tests/serve/test_issue_3167.cpp",
    "tests/core/test_issue_3167.cpp",
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
    // src/core/persistent_child_vector.hh
    namespace aura::ast {
    // Issue #3167: 6-arg ctor captures FlatAST
    SafePCVSpan(std::span<const T> sp,
                std::shared_ptr<const Storage> keep,
                std::uint32_t node_id,
                std::uint64_t generation,
                std::uint32_t wrap_epoch,
                std::uint16_t node_gen);
    bool is_stale(std::uint64_t current_generation,
                  std::uint32_t current_wrap_epoch,
                  std::uint16_t current_node_gen);
    std::uint32_t captured_node_id();
    std::uint64_t captured_generation();
    std::uint32_t captured_wrap_epoch();
    std::uint16_t captured_node_gen();
    bool has_fingerprint();
    static_assert(safe_pcv_fingerprint_shape<SafePCVSpan<...>>(), "...");
    inline constexpr std::uint32_t kPcvSpanNoOwner = UINT32_MAX;
    }
    // src/core/ast.ixx
    force_refresh_pcv_span(safe, id);
    pcv_span_stale_across_guard_total();
    // Issue #3167: capture FlatAST node_id + generation_
    // tests/core/test_pcv_exclusive_with_set.cpp
    ac3167_1_production_stale_refresh();
    ac3167_2_happy_path_zero_extra();
    ac3167_4_additive_counter_only();
    // tests/compiler/test_hygiene_mutate_closed_loop.cpp
    ac3167_3_2906_non_regression();
    ac3167_6_source_and_linter();
    // tests/core/test_pcv_workspace_batch.cpp
    // Issue #3167: SafePCVSpan stale-across-guard batch smoke check
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
        description="Issue #3167 SafePCVSpan stale-across-guard source-cite gate",
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
        help="Only check forbidden artifacts (docs/design/3167-*, test_issue_3167.cpp)",
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

    print("OK: Issue #3167 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
