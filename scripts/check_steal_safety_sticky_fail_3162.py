#!/usr/bin/env python3
# scripts/check_steal_safety_sticky_fail_3162.py -- Issue #3162 source-cite gate.
#
# Verifies the production-readiness residual sticky-fail bit is wired
# across the three layers:
#
#  1. Atomic + accessor + clear-on-zero (src/serve/steal_safety.h):
#     - g_steal_safety_production_residual_sticky_fail atomic (default 0)
#     - g_steal_safety_production_residual_sticky_fail_wired sentinel (default 1)
#     - kStealSafetyProductionResidualStickyFailIssue = 3162
#     - steal_safety_production_residual_sticky_fail_v_read() accessor
#     - steal_safety_production_residual_zero_v_read() clears sticky_fail
#       when residual returns to 0 (per-query poll)
#     - steal_safety_production_residual_sticky_fail_wired_v_read() accessor
#     - clear_steal_safety_transaction_for_test() resets sticky_fail
#
#  2. Set site (src/serve/steal_safety.cpp steal_safety_transaction):
#     - RejectHard residual-fail branch sets sticky_fail.store(1) under
#       production when steal_safety_production_residual_zero_v_read() == 0
#     - Quiet Ok path (ticket stamp + return Ok): no sticky_fail touch
#       (zero extra atomics on hot path)
#
#  3. Schema surface (src/compiler/evaluator_primitives_query_type_stats.cpp):
#     - production-readiness-steal-residual-sticky-fail additive key
#     - schema-3162 / issue-3162 additive keys
#     - existing #3134 keys (schema-3134 / issue-3134 / production-
#       readiness-steal-residual-zero) non-regressing
#
#  4. Test extension (tests/serve/test_steal_safety_production_residual_zero.cpp):
#     - AC6 sticky-fail wired (atomic + accessor + schema + set site)
#     - AC7 Soft pass-through (accessor returns 0 when production off)
#     - AC8 Ok path zero extra atomics (sticky_fail.store AFTER Ok stamp)
#     - AC9 #3134 non-regression + additive schema-3162
#
#  5. Forbidden artifacts (per #1655 + #81967):
#     - No docs/design/3162-* plan doc
#     - No tests/issues/test_issue_3162.cpp
#     - No tests/serve/test_issue_3162.cpp

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/serve/steal_safety.h",
    "src/serve/steal_safety.cpp",
    "src/compiler/evaluator_primitives_query_type_stats.cpp",
    "tests/serve/test_steal_safety_production_residual_zero.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Layer 1: steal_safety.h atomic + accessor + clear-on-zero
    (
        "src/serve/steal_safety.h",
        r"inline\s+std::atomic<std::uint32_t>\s+"
        r"g_steal_safety_production_residual_sticky_fail\{0\}",
        "sticky_fail atomic (default 0)",
    ),
    (
        "src/serve/steal_safety.h",
        r"inline\s+std::atomic<std::uint32_t>\s+"
        r"g_steal_safety_production_residual_sticky_fail_wired\{1\}",
        "sticky_fail_wired sentinel (default 1)",
    ),
    (
        "src/serve/steal_safety.h",
        r"inline\s+constexpr\s+int\s+"
        r"kStealSafetyProductionResidualStickyFailIssue\s*=\s*3162",
        "kStealSafetyProductionResidualStickyFailIssue = 3162",
    ),
    (
        "src/serve/steal_safety.h",
        r"\[\[nodiscard\]\]\s+inline\s+std::uint32_t\s+"
        r"steal_safety_production_residual_sticky_fail_v_read\(\)",
        "sticky_fail_v_read accessor decl",
    ),
    (
        "src/serve/steal_safety.h",
        r"g_steal_safety_production_residual_sticky_fail\.store\(0,\s*std::memory_order_relaxed\)",
        "clear-on-zero (sticky_fail.store(0) in residual_zero_v_read + test reset)",
    ),
    (
        "src/serve/steal_safety.h",
        r"\[\[nodiscard\]\]\s+inline\s+std::uint32_t\s+"
        r"steal_safety_production_residual_sticky_fail_wired_v_read\(\)",
        "sticky_fail_wired_v_read accessor decl",
    ),
    # Layer 2: steal_safety.cpp set site in RejectHard branch
    (
        "src/serve/steal_safety.cpp",
        r"g_steal_safety_production_residual_sticky_fail\.store\(1,\s*std::memory_order_relaxed\)",
        "sticky_fail.store(1) in RejectHard residual-fail branch",
    ),
    (
        "src/serve/steal_safety.cpp",
        r"if\s*\(\s*steal_safety_production_residual_zero_v_read\(\)\s*==\s*0\s*\)\s*\{[^}]*"
        r"sticky_fail\.store\(1",
        "sticky_fail.store(1) gated by residual_zero_v_read() == 0",
    ),
    (
        "src/serve/steal_safety.cpp",
        r"//\s*Issue\s+#3162:\s*production multi-worker sticky readiness-fail",
        "Issue #3162 comment in steal_safety.cpp",
    ),
    # Layer 3: schema surface additive keys
    (
        "src/compiler/evaluator_primitives_query_type_stats.cpp",
        r'production-readiness-steal-"\n\s*"residual-sticky-fail',
        "schema-3073 sticky_fail key (split literal)",
    ),
    (
        "src/compiler/evaluator_primitives_query_type_stats.cpp",
        r"steal_safety_production_residual_sticky_fail_v_read\(\)",
        "accessor call in schema surface",
    ),
    (
        "src/compiler/evaluator_primitives_query_type_stats.cpp",
        r'insert_kv\("schema-3162",\s*3162\)',
        "schema-3162 key",
    ),
    (
        "src/compiler/evaluator_primitives_query_type_stats.cpp",
        r'insert_kv\("issue-3162",\s*3162\)',
        "issue-3162 key",
    ),
    # Layer 3: existing #3134 keys non-regressing
    (
        "src/compiler/evaluator_primitives_query_type_stats.cpp",
        r'insert_kv\("schema-3134",\s*3134\)',
        "schema-3134 key (non-regression)",
    ),
    (
        "src/compiler/evaluator_primitives_query_type_stats.cpp",
        r'insert_kv\("issue-3134",\s*3134\)',
        "issue-3134 key (non-regression)",
    ),
    (
        "src/compiler/evaluator_primitives_query_type_stats.cpp",
        r'production-readiness-steal-"\n\s*"residual-zero',
        "production-readiness-steal-residual-zero key (split literal, non-regression)",
    ),
    # Layer 4: test extension AC6-AC9 (flexible: matches either header or block comment)
    (
        "tests/serve/test_steal_safety_production_residual_zero.cpp",
        r"AC6[^\n]*Issue\s+#3162",
        "AC6 test (Issue #3162)",
    ),
    (
        "tests/serve/test_steal_safety_production_residual_zero.cpp",
        r"AC7[^\n]*Issue\s+#3162",
        "AC7 test (Issue #3162)",
    ),
    (
        "tests/serve/test_steal_safety_production_residual_zero.cpp",
        r"AC8[^\n]*Issue\s+#3162",
        "AC8 test (Issue #3162)",
    ),
    (
        "tests/serve/test_steal_safety_production_residual_zero.cpp",
        r"AC9[^\n]*Issue\s+#3162",
        "AC9 test (Issue #3162)",
    ),
    (
        "tests/serve/test_steal_safety_production_residual_zero.cpp",
        r"steal_safety_production_residual_sticky_fail\.store\(1",
        "sticky_fail.store(1) grep in test (AC6 set-site source-cite)",
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
    """Verify no docs/design/3162-* (#1655) or test_issue_3162.cpp (#81967)."""
    failures: list[str] = []
    design_dir = REPO_ROOT / "docs" / "design"
    if design_dir.exists():
        for f in design_dir.iterdir():
            if f.name.startswith("3162-"):
                failures.append(f"docs/design/{f.name}: forbidden per #1655")
    issues_dir = REPO_ROOT / "tests" / "issues"
    if issues_dir.exists():
        target = issues_dir / "test_issue_3162.cpp"
        if target.exists():
            failures.append("tests/issues/test_issue_3162.cpp: forbidden per #81967")
    serve_issues = REPO_ROOT / "tests" / "serve" / "test_issue_3162.cpp"
    if serve_issues.exists():
        failures.append("tests/serve/test_issue_3162.cpp: forbidden per #81967")
    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_steal_safety_h = """
    inline std::atomic<std::uint32_t> g_steal_safety_production_residual_sticky_fail{0};
    inline std::atomic<std::uint32_t> g_steal_safety_production_residual_sticky_fail_wired{1};
    inline constexpr int kStealSafetyProductionResidualStickyFailIssue = 3162;
    [[nodiscard]] inline std::uint32_t
    steal_safety_production_residual_sticky_fail_v_read() noexcept {
        if (aura_production_defaults_active_probe() == 0)
            return 0;
        return g_steal_safety_production_residual_sticky_fail.load(std::memory_order_relaxed);
    }
    [[nodiscard]] inline std::uint32_t
    steal_safety_production_residual_sticky_fail_wired_v_read() noexcept {
        return g_steal_safety_production_residual_sticky_fail_wired.load(std::memory_order_relaxed);
    }
    // clear on zero (residual_zero_v_read body)
    g_steal_safety_production_residual_sticky_fail.store(0, std::memory_order_relaxed);
    // test reset
    g_steal_safety_production_residual_sticky_fail.store(0, std::memory_order_relaxed);
    """
    fixture_steal_safety_cpp = """
    if (!residual_ok) {
        // Issue #3162: production multi-worker sticky readiness-fail.
        if (steal_safety_production_residual_zero_v_read() == 0) {
            g_steal_safety_production_residual_sticky_fail.store(1, std::memory_order_relaxed);
        }
    }
    """
    fixture_qts = """
    insert_kv(
        "production-readiness-steal-"
        "residual-sticky-fail",
        static_cast<std::int64_t>(
            aura::serve::steal_safety_production_residual_sticky_fail_v_read()));
    insert_kv("schema-3073", 3073);
    insert_kv("issue-3073", 3073);
    insert_kv("schema-3134", 3134);
    insert_kv("issue-3134", 3134);
    insert_kv("schema-3162", 3162);
    insert_kv("issue-3162", 3162);
    insert_kv(
        "production-readiness-steal-"
        "residual-zero",
        static_cast<std::int64_t>(...));
    """
    fixture_test = """
    //   AC6 (Issue #3162): sticky-fail atomic wired + accessor +
    //   AC7 (Issue #3162): Soft / sandbox=off / single-worker: zero
    //   AC8 (Issue #3162): quiet Ok path: zero extra atomics — bit only set
    //   AC9 (Issue #3162): existing #3134 accessor + schema-3073 keys
    must_inline(fn_win, "steal_safety_production_residual_sticky_fail.store(1");
    """
    fixtures = {
        "src/serve/steal_safety.h": fixture_steal_safety_h,
        "src/serve/steal_safety.cpp": fixture_steal_safety_cpp,
        "src/compiler/evaluator_primitives_query_type_stats.cpp": fixture_qts,
        "tests/serve/test_steal_safety_production_residual_zero.cpp": fixture_test,
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
        description="Issue #3162 production-readiness residual sticky-fail source-cite gate",
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
        help="Only check forbidden artifacts (docs/design/3162-*, test_issue_3162.cpp)",
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

    print("OK: Issue #3162 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
