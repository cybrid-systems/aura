#!/usr/bin/env python3
# scripts/check_dual_dep_graph_parity_strict_fail_closed_3165.py -- Issue #3165 source-cite gate.
#
# Verifies the dual DepGraph parity fail under Strict fails-closed to force
# dirty of ALL callers in the graph (not just current callee's called_by),
# closing the race with deferred hybrid drain + concurrent partial peel.
#
#  1. Producer hook #1 (src/compiler/service.ixx record_dependency):
#     - Strict branch walks all callers in dep_graph_ (set-deduped)
#       under the same exclusive dep_graph_mtx_ window as the rebuild
#     - Cite Issue #3165 in the Strict branch
#     - Existing dual_dep_graph_parity_fail_total counter reused
#
#  2. Producer hook #2 (src/compiler/service.ixx drain_deferred_hybrid_cascade_):
#     - Strict branch force-dirty all callers after rebuild
#     - Cite Issue #3165
#     - Soft / off (non-Strict): rebuild only, zero extra force
#
#  3. Test extension (tests/compiler/test_dep_graph_hybrid_cascade.cpp):
#     - ac3165_strict_fail_closed_all_callers() function present
#     - ac3165 registered in run_test_dep_graph_hybrid_cascade()
#     - All-callers walk via "for (const auto& [callee_name, callee_entry] : dep_graph_)"
#     - Source-cite: Issue #3165 + dual_dep_graph_strict_enabled + rebuild call
#     - Forbidden: no test_issue_3165.cpp, no docs/design/3165-*

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/service.ixx",
    "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Layer 1: service.ixx record_dependency Strict all-callers walk
    (
        "src/compiler/service.ixx",
        r"//\s*Issue\s+#3165:\s*Strict mode\s*\(production\)\s*must fail-closed",
        "Issue #3165 comment in record_dependency",
    ),
    (
        "src/compiler/service.ixx",
        r"for\s*\(\s*const auto&\s*\[callee_name,\s*callee_entry\]\s*:\s*dep_graph_\s*\)",
        "all-callers walk via dep_graph_ iteration",
    ),
    (
        "src/compiler/service.ixx",
        r"std::unordered_set<std::string,\s*aura::core::TransparentStringHash",
        "set-dedup affected callers",
    ),
    (
        "src/compiler/service.ixx",
        r"affected\.insert\(caller_name\)",
        "collect unique caller names",
    ),
    # Layer 2: service.ixx drain_deferred_hybrid_cascade_ Strict branch
    (
        "src/compiler/service.ixx",
        r"//\s*Issue\s+#3165:\s*under Strict,\s*fail-closed force dirty of",
        "Issue #3165 comment in drain_deferred_hybrid_cascade_",
    ),
    (
        "src/compiler/service.ixx",
        r"if\s*\(\s*aura::compiler::dirty::dual_dep_graph_strict_enabled\(\)\s*\)",
        "dual_dep_graph_strict_enabled gate (both branches)",
    ),
    # Layer 3: existing #2247 invariants non-regressing
    (
        "src/compiler/service.ixx",
        r"dual_dep_graph_parity_fail_total\.fetch_add\(1,\s*std::memory_order_relaxed\)",
        "existing parity_fail counter reused",
    ),
    (
        "src/compiler/service.ixx",
        r"rebuild_node_dep_graph_from_string",
        "rebuild helper still called before Strict force-dirty",
    ),
    # Layer 4: test extension AC3165
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"ac3165_strict_fail_closed_all_callers",
        "ac3165 function + registration present",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"//\s*Issue\s+#3165:\s*dual DepGraph parity fail under Strict",
        "AC3165 source-cite header comment",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"3165:\s*record_dependency cite Issue\s+#3165",
        "AC3165 record_dependency cite check",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"3165:\s*record_dependency Strict gate",
        "AC3165 record_dependency Strict gate check",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"3165:\s*record_dependency walks all callers in dep_graph_",
        "AC3165 all-callers walk check",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"3165:\s*drain cite Issue\s+#3165",
        "AC3165 drain cite check",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"3165:\s*drain Strict gate",
        "AC3165 drain Strict gate check",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"3165:\s*drain walks all callers in dep_graph_",
        "AC3165 drain all-callers walk check",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"3165:\s*record_dependency still calls rebuild before Strict force-dirty",
        "AC3165 record_dependency rebuild-before-force-dirty",
    ),
    (
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp",
        r"3165:\s*existing parity_fail counter reused",
        "AC3165 existing parity_fail counter non-regression",
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
    """Verify no docs/design/3165-* (#1655) or test_issue_3165.cpp (#81967)."""
    failures: list[str] = []
    design_dir = REPO_ROOT / "docs" / "design"
    if design_dir.exists():
        for f in design_dir.iterdir():
            if f.name.startswith("3165-"):
                failures.append(f"docs/design/{f.name}: forbidden per #1655")
    issues_dir = REPO_ROOT / "tests" / "issues"
    if issues_dir.exists():
        target = issues_dir / "test_issue_3165.cpp"
        if target.exists():
            failures.append("tests/issues/test_issue_3165.cpp: forbidden per #81967")
    compiler_issues = REPO_ROOT / "tests" / "compiler" / "test_issue_3165.cpp"
    if compiler_issues.exists():
        failures.append("tests/compiler/test_issue_3165.cpp: forbidden per #81967")
    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_svc = """
    if (!aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_,
                                                  dep_name_to_slot_)) {
        metrics_.dual_dep_graph_parity_fail_total.fetch_add(1, std::memory_order_relaxed);
        aura::compiler::dirty::rebuild_node_dep_graph_from_string(node_dep_graph_, dep_graph_,
                                                                  dep_name_to_slot_);
        // Issue #3165: Strict mode (production) must fail-closed force
        // dirty of ALL callers in the graph
        if (aura::compiler::dirty::dual_dep_graph_strict_enabled()) {
            std::unordered_set<std::string, aura::core::TransparentStringHash,
                               std::equal_to<>>
                affected;
            for (const auto& [callee_name, callee_entry] : dep_graph_) {
                (void)callee_name;
                for (const auto& caller_name : callee_entry.called_by) {
                    affected.insert(caller_name);
                }
            }
        }
    }
    // drain_deferred_hybrid_cascade_ parity branch
    if (!aura::compiler::dirty::graphs_consistent(...)) {
        aura::compiler::dirty::rebuild_node_dep_graph_from_string(...);
        metrics_.dual_dep_graph_parity_fail_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #3165: under Strict, fail-closed force dirty of
        // ALL callers
        if (aura::compiler::dirty::dual_dep_graph_strict_enabled()) {
            for (const auto& [callee_name, callee_entry] : dep_graph_) {
                ...
            }
        }
    }
    """
    fixture_test = """
    // Issue #3165: dual DepGraph parity fail under Strict must fail-closed
    static void ac3165_strict_fail_closed_all_callers() {
        auto svc = read_file("src/compiler/service.ixx");
        auto rd_pos = svc.find("void record_dependency(const std::string& caller, const std::string& callee)");
        ...
        CHECK(rd_win.find("Issue #3165") != std::string::npos,
              "3165: record_dependency cite Issue #3165");
        CHECK(rd_win.find("dual_dep_graph_strict_enabled") != std::string::npos,
              "3165: record_dependency Strict gate");
        CHECK(rd_win.find("for (const auto& [callee_name, callee_entry] : dep_graph_)") !=
                  std::string::npos,
              "3165: record_dependency walks all callers in dep_graph_");
        auto drain_pos = svc.find("void drain_deferred_hybrid_cascade_()");
        ...
        CHECK(drain_win.find("Issue #3165") != std::string::npos,
              "3165: drain cite Issue #3165");
        CHECK(drain_win.find("dual_dep_graph_strict_enabled") != std::string::npos,
              "3165: drain Strict gate");
        CHECK(drain_win.find("for (const auto& [callee_name, callee_entry] : dep_graph_)") !=
                  std::string::npos,
              "3165: drain walks all callers in dep_graph_");
        CHECK(rd_win.find("rebuild_node_dep_graph_from_string") != std::string::npos,
              "3165: record_dependency still calls rebuild before Strict force-dirty");
        CHECK(rd_win.find("dual_dep_graph_parity_fail_total") != std::string::npos,
              "3165: existing parity_fail counter reused (no new metric key)");
    }
    int run_test_dep_graph_hybrid_cascade() {
        ac3067_4_soak_and_linter();
        ac3165_strict_fail_closed_all_callers();
    }
    """
    fixtures = {
        "src/compiler/service.ixx": fixture_svc,
        "tests/compiler/test_dep_graph_hybrid_cascade.cpp": fixture_test,
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
        description="Issue #3165 dual DepGraph parity Strict fail-closed source-cite gate",
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
        help="Only check forbidden artifacts (docs/design/3165-*, test_issue_3165.cpp)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures: list[str] = []

    if not args.forbidden_only:
        for rel, rx, _label in INFRA_REQUIRED:
            failures.extend(check_file(rel, rx, strict=args.strict))

    if failures:
        print(f"FAIL: {len(failures)} issue(s):")
        for line in failures:
            print(f"  {line}")
        return 1

    print("OK: Issue #3165 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
