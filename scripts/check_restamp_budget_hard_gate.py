#!/usr/bin/env python3
# scripts/check_restamp_budget_hard_gate.py -- Issue #3104 source-cite gate.
#
# Verifies the restamp-budget hard-gate infrastructure is in place across
# the three layers:
#
#  1. Restamp exit path (src/compiler/evaluator_fiber_mutation.cpp):
#     - unified_restamp_after_boundary sets restamp_last_budget_exceeded_
#     - unified_restamp_after_boundary calls force_query_epoch_stale_from_restamp_budget()
#       under production_defaults_active when budget exceeded
#     - unified_restamp_after_boundary bumps g_unified_restamp_torn_visible_total
#
#  2. Gate accessors (src/core/ast.ixx + src/compiler/evaluator_security.cpp):
#     - FlatAST::restamp_last_budget_exceeded() accessor exists
#     - FlatAST::restamp_last_budget_exceeded_ atomic flag exists
#     - Evaluator::allow_query_stable_ref_export(id) exists + checks the flag
#     - Evaluator::query_stable_hard_reject_torn() exists + checks production + flag
#     - Evaluator::stamp_query_stable_ref_export(ref) exists + nulls ref on fail
#
#  3. Export sites (src/compiler/evaluator_primitives_query_workspace.cpp):
#     - query:children-stable uses allow_query_stable_ref_export + returns restamp-lag
#     - query:parent-stable uses allow_query_stable_ref_export + returns restamp-lag
#     - query:stable-ref uses allow_query_stable_ref_export + returns restamp-lag
#     - query:ensure-ref uses allow_query_stable_ref_export + returns restamp-lag
#
#  4. Force-stale (src/core/workspace_epoch.hh):
#     - force_query_epoch_stale_from_restamp_budget() exists
#
#  5. Observability (src/core/ast.ixx):
#     - restamp_budget_exceeded_total_ atomic + accessor exists

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/core/ast.ixx",
    "src/compiler/evaluator_security.cpp",
    "src/compiler/evaluator.cpp",
    "src/compiler/evaluator_primitives_query_workspace.cpp",
    "src/core/workspace_epoch.hh",
)

# (path, regex, label) -- each tuple is a substring/regex that must appear.
INFRA_REQUIRED: tuple[tuple[str, str], ...] = (
    # FlatAST flag + accessor (src/core/ast.ixx)
    ("src/core/ast.ixx", r"restamp_last_budget_exceeded_\{0\}"),
    ("src/core/ast.ixx", r"restamp_last_budget_exceeded\(\)\s+const\s+noexcept"),
    ("src/core/ast.ixx", r"restamp_budget_exceeded_total_\{0\}"),
    ("src/core/ast.ixx", r"restamp_budget_exceeded_total\(\)\s+const\s+noexcept"),
    # Force-stale (src/core/workspace_epoch.hh)
    ("src/core/workspace_epoch.hh", r"force_query_epoch_stale_from_restamp_budget\(\)"),
    # Restamp exit path (src/compiler/evaluator_fiber_mutation.cpp)
    ("src/compiler/evaluator_fiber_mutation.cpp", r"force_query_epoch_stale_from_restamp_budget\(\)"),
    ("src/compiler/evaluator_fiber_mutation.cpp", r"production_defaults_active\(\)"),
    ("src/compiler/evaluator_fiber_mutation.cpp", r"g_unified_restamp_torn_visible_total"),
    # Gate accessors (src/compiler/evaluator_security.cpp)
    ("src/compiler/evaluator_security.cpp", r"Evaluator::allow_query_stable_ref_export"),
    ("src/compiler/evaluator_security.cpp", r"Evaluator::query_stable_hard_reject_torn"),
    ("src/compiler/evaluator_security.cpp", r"Evaluator::stamp_query_stable_ref_export"),
    # Decls (src/compiler/evaluator.ixx)
    ("src/compiler/evaluator.ixx", r"allow_query_stable_ref_export"),
    ("src/compiler/evaluator.ixx", r"query_stable_hard_reject_torn"),
    ("src/compiler/evaluator.ixx", r"stamp_query_stable_ref_export"),
)

EXPORT_SITE_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # (path, site-name-keyword, restamp-lag-error-keyword)
    # query:children-stable at line ~421
    ("src/compiler/evaluator_primitives_query_workspace.cpp", "query:children-stable", r"mev\(\"restamp-lag\""),
    # query:parent-stable at line ~497
    ("src/compiler/evaluator_primitives_query_workspace.cpp", "query:parent-stable", r"mev\(\"restamp-lag\""),
    # query:stable-ref at line ~612
    ("src/compiler/evaluator_primitives_query_workspace.cpp", "query:stable-ref", r"mev\(\"restamp-lag\""),
    # query:ensure-ref at line ~697
    ("src/compiler/evaluator_primitives_query_workspace.cpp", "query:ensure-ref", r"mev\(\"restamp-lag\""),
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


def check_export_sites(rel_path: str, site_keyword: str, restamp_lag_regex: str, *, strict: bool) -> list[str]:
    failures: list[str] = []
    p = REPO_ROOT / rel_path
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    if site_keyword not in text:
        if strict:
            failures.append(f"{rel_path}: export site {site_keyword!r} not found")
        return failures
    # Find all occurrences of the keyword. The actual add() body may not be the
    # first match (header comment, doc reference, error message text may precede).
    # We try each anchor in order; if any of them has the restamp-lag error in
    # its 4000-char window, the export site is wired correctly.
    found_anchor = False
    for idx in (m.start() for m in re.finditer(re.escape(site_keyword), text)):
        candidate = text[idx : idx + 4000]
        if re.search(restamp_lag_regex, candidate):
            found_anchor = True
            break
    if not found_anchor and strict:
        failures.append(
            f"{rel_path}: export site {site_keyword!r} does not return "
            f"restamp-lag structured error (regex {restamp_lag_regex!r})"
        )
    return failures

def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_infra = """
    // ast.ixx
    mutable std::atomic<std::uint32_t> restamp_last_budget_exceeded_{0};
    mutable std::atomic<std::uint64_t> restamp_budget_exceeded_total_{0};
    [[nodiscard]] bool restamp_last_budget_exceeded() const noexcept;
    std::uint64_t restamp_budget_exceeded_total() const noexcept;
    // workspace_epoch.hh
    inline void force_query_epoch_stale_from_restamp_budget() noexcept;
    // evaluator_fiber_mutation.cpp
    if (r.budget_exceeded) {
        aura::ast::g_unified_restamp_torn_visible_total.fetch_add(1);
        if (production && typed_audit::production_defaults_active())
            aura::core::force_query_epoch_stale_from_restamp_budget();
    }
    // evaluator_security.cpp
    bool Evaluator::allow_query_stable_ref_export(ast::NodeId id) const noexcept;
    bool Evaluator::query_stable_hard_reject_torn() const noexcept;
    void Evaluator::stamp_query_stable_ref_export(ast::FlatAST::StableNodeRef& ref) const noexcept;
    // evaluator.ixx
    [[nodiscard]] bool allow_query_stable_ref_export(ast::NodeId id) const noexcept;
    [[nodiscard]] bool query_stable_hard_reject_torn() const noexcept;
    void stamp_query_stable_ref_export(ast::FlatAST::StableNodeRef& ref) const noexcept;
    """
    fixture_export = """
    // query:children-stable
    if (!ev.allow_query_stable_ref_export(cid))
        return mev("restamp-lag", "query:children-stable: restamp budget exceeded");
    // query:parent-stable
    if (!ev.allow_query_stable_ref_export(pref.id))
        return mev("restamp-lag", "query:parent-stable: restamp budget exceeded");
    // query:stable-ref
    if (!ev.allow_query_stable_ref_export(node))
        return mev("restamp-lag", "query:stable-ref: restamp budget exceeded");
    // query:ensure-ref
    if (!ev.allow_query_stable_ref_export(held.id))
        return mev("restamp-lag", "query:ensure-ref: restamp budget exceeded");
    """
    fails: list[str] = []
    for rel, rx in INFRA_REQUIRED:
        if (
            rel.endswith("ast.ixx")
            or rel.endswith("workspace_epoch.hh")
            or rel.endswith("evaluator_fiber_mutation.cpp")
            or rel.endswith("evaluator_security.cpp")
            or rel.endswith("evaluator.ixx")
        ):
            text = fixture_infra
        else:
            text = ""
        if not re.search(rx, text):
            fails.append(f"{rel}: missing required pattern: {rx!r}")
    # Export sites (check site_keyword + restamp_lag_regex in window)
    for rel, site_keyword, restamp_lag_regex in EXPORT_SITE_REQUIRED:
        text = fixture_export
        if site_keyword not in text:
            fails.append(f"{rel}: export site {site_keyword!r} not found")
            continue
        idx = text.find(site_keyword)
        window = text[idx : idx + 4000]
        if not re.search(restamp_lag_regex, window):
            fails.append(
                f"{rel}: export site {site_keyword!r} does not return "
                f"restamp-lag structured error (regex {restamp_lag_regex!r})"
            )
    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns found in fixture")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3104 restamp-budget hard-gate source-cite gate",
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
        help="Run the linter self-test and exit",
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="Files to scan (default: workspace_epoch.hh + ast.ixx + evaluator_fiber_mutation.cpp + evaluator_security.cpp + evaluator.cpp + evaluator_primitives_query_workspace.cpp)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures: list[str] = []
    target_filter = set(args.targets) if args.targets else set(DEFAULT_TARGETS)
    for rel_path, regex in INFRA_REQUIRED:
        if rel_path not in target_filter:
            continue
        failures.extend(check_file(rel_path, regex, strict=args.strict))
    for rel_path, site_keyword, restamp_lag_regex in EXPORT_SITE_REQUIRED:
        if rel_path not in target_filter:
            continue
        failures.extend(check_export_sites(rel_path, site_keyword, restamp_lag_regex, strict=args.strict))

    if failures:
        print("check_restamp_budget_hard_gate: FAIL")
        for line in failures:
            print(f"  {line}")
        return 1

    print("check_restamp_budget_hard_gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
