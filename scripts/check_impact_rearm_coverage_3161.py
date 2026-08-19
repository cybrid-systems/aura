#!/usr/bin/env python3
# scripts/check_impact_rearm_coverage_3161.py -- Issue #3161 source-cite gate.
#
# Verifies the impact_upper_bound under-estimate residual (post-#3097/#3068)
# hardening is in place:
#
#  1. Producer hook (src/compiler/service.ixx relower_dirty_defines_from_workspace):
#     - initial_node_mirror_edges snapshot under cascade_decision_mtx_ critical
#       section (line ~6859)
#     - rearm_observed_mid_loop flag scoped to dirty_names loop (line ~6861)
#     - rearmed_now / graph_grew_mid_loop dual-signal re-check (line ~6952)
#     - rearm_observed_mid_loop cascade to remaining defines (line ~6958)
#
#  2. Metric reuse (AC2 contract): partial_forced_full_by_impact_total captures
#     the upgrade — no new middle-of-metrics keys introduced.
#
#  3. Soft / Off zero-cost invariant (AC3 contract): the lock is gated on
#     `need_lock = initial_armed || production_defaults_active()` and only
#     acquired when need_lock is true (defer_lock pattern preserved from #3135).
#
#  4. Test extension (AC4 contract): AC9 concurrent rearm during peel soak
#     added to test_partial_relower_cascade.cpp; no tests/issues/test_issue_3161.cpp
#     (#81967); no docs/design/3161-* (#1655).

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/service.ixx",
    "tests/compiler/test_partial_relower_cascade.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Producer hook (src/compiler/service.ixx relower_dirty_defines_from_workspace)
    (
        "src/compiler/service.ixx",
        r"const\s+auto\s+initial_node_mirror_edges\s*=\s*"
        r"metrics_\.dep_graph_node_mirror_edges_total\.load",
        "initial_node_mirror_edges snapshot under lock",
    ),
    (
        "src/compiler/service.ixx",
        r"bool\s+rearm_observed_mid_loop\s*=\s*false",
        "rearm_observed_mid_loop flag scoped to loop",
    ),
    (
        "src/compiler/service.ixx",
        r"const\s+bool\s+graph_grew_mid_loop\s*=\s*"
        r"metrics_\.dep_graph_node_mirror_edges_total\.load\(\s*std::memory_order_relaxed\s*\)"
        r"\s*>\s*initial_node_mirror_edges",
        "graph_grew_mid_loop dual-signal re-check",
    ),
    (
        "src/compiler/service.ixx",
        r"if\s*\(\s*rearm_observed_mid_loop\s*&&\s*want_partial\s*\)",
        "rearm_observed_mid_loop cascade to remaining defines",
    ),
    # AC2: existing metric reused, no new middle-of-metrics keys
    (
        "src/compiler/service.ixx",
        r"metrics_\.partial_forced_full_by_impact_total\.fetch_add\(\s*1,\s*std::memory_order_relaxed\)",
        "partial_forced_full_by_impact_total reused (no new metric key)",
    ),
    # AC3: Soft / Off zero-cost — need_lock gate preserved from #3135
    (
        "src/compiler/service.ixx",
        r"const\s+bool\s+need_lock\s*=\s*initial_armed\s*\|\|"
        r"\s*aura::compiler::typed_audit::production_defaults_active\(\)",
        "need_lock gate (Soft skip preserved)",
    ),
    # Test extension (AC4) — AC9 concurrent rearm soak added to existing suite
    (
        "tests/compiler/test_partial_relower_cascade.cpp",
        r"void\s+ac9_concurrent_rearm_soak\(\)",
        "ac9_concurrent_rearm_soak test function",
    ),
    (
        "tests/compiler/test_partial_relower_cascade.cpp",
        r"ac9_concurrent_rearm_soak\(\);",
        "ac9 registered in run_test_partial_relower_cascade",
    ),
    (
        "tests/compiler/test_partial_relower_cascade.cpp",
        r"dep_graph_node_mirror_edges_total",
        "ac9 observes node_mirror counter (rearm signal source)",
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
    """Verify no docs/design/3161-* (#1655) or tests/issues/test_issue_3161.cpp (#81967)."""
    failures: list[str] = []
    design_dir = REPO_ROOT / "docs" / "design"
    if design_dir.exists():
        for f in design_dir.iterdir():
            if f.name.startswith("3161-"):
                failures.append(f"docs/design/{f.name}: forbidden per #1655")
    issues_dir = REPO_ROOT / "tests" / "issues"
    if issues_dir.exists():
        target = issues_dir / "test_issue_3161.cpp"
        if target.exists():
            failures.append("tests/issues/test_issue_3161.cpp: forbidden per #81967")
    compiler_issues = REPO_ROOT / "tests" / "compiler" / "test_issue_3161.cpp"
    if compiler_issues.exists():
        failures.append("tests/compiler/test_issue_3161.cpp: forbidden per #81967")
    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_service_ixx = """
    const auto initial_node_mirror_edges =
        metrics_.dep_graph_node_mirror_edges_total.load(std::memory_order_relaxed);
    bool rearm_observed_mid_loop = false;
    const bool need_lock =
        initial_armed || aura::compiler::typed_audit::production_defaults_active();
    // ...
    const bool graph_grew_mid_loop =
        metrics_.dep_graph_node_mirror_edges_total.load(std::memory_order_relaxed) >
        initial_node_mirror_edges;
    if (rearm_observed_mid_loop && want_partial) {
        want_partial = false;
        metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
    }
    """
    fixture_test = """
    void ac9_concurrent_rearm_soak() {
        const auto node_mirror0 =
            m->dep_graph_node_mirror_edges_total.load(std::memory_order_relaxed);
    }
    int run_test_partial_relower_cascade() {
        ac9_concurrent_rearm_soak();
    }
    """
    fails: list[str] = []
    for rel, rx, label in INFRA_REQUIRED:
        if "service.ixx" in rel:
            text = fixture_service_ixx
        elif "test_partial_relower_cascade.cpp" in rel:
            text = fixture_test
        else:
            text = ""
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
        description="Issue #3161 impact_upper_bound re-arm observation source-cite gate",
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
        help="Only check forbidden artifacts (docs/design/3161-*, test_issue_3161.cpp)",
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

    print("OK: Issue #3161 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
