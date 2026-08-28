#!/usr/bin/env python3
# scripts/check_query_stable_hard_reject_torn_latch_3386.py -- Issue #3386 source-cite gate.
#
# Verifies the shared restamp-status probe
# Evaluator::query_stable_hard_reject_torn() observes the same face as
# allow_query_stable_ref_export (over-budget torn bit) and consults the
# process latch (independent of the flip-able production_defaults_active()):
#
#  AC1: probe predicate ORs restamp_over_budget_torn() — production/latched +
#       residual torn on nodes outside the hot cone still flips the probe
#       true even when restamp_last_budget_exceeded() is cleared.
#  AC2: hard gate ORs aura_runtime_multi_worker_production_latched() — the
#       process latch is sticky post-Ready, so a Soft flip of
#       production_defaults_active() after latch keeps the probe active.
#  AC3: Soft + unlatched + torn → probe false (no extra beyond the existing
#       defaults load — short-circuit preserves zero-cost quiet path).
#  AC4: allow_query_stable_ref_export node-level eager-bit allow is
#      preserved (hot-cone eagerly restamped node stays exportable; the
#       probe may be true while workspace is torn without forcing that
#       specific node green).
#  AC5: Source-cite only. No docs/design/3386-* (per MEMORY 2026-07-19),
#      no tests/issues/test_issue_3386.cpp (#81967). Existing #3100 /
#      #3138 / #3230 / #3287 / #3309 suites green.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = ("src/compiler/evaluator_security.cpp",)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: probe predicate ORs restamp_over_budget_torn().
    (
        "src/compiler/evaluator_security.cpp",
        r"return\s+ws->restamp_last_budget_exceeded\(\)\s*\|\|\s*ws->nested_authority_gap\(\)\s*\|\|[\s\S]{0,40}ws->restamp_over_budget_torn\(\)",
        "3386 AC1: probe predicate ORs restamp_over_budget_torn()",
    ),
    # AC2: hard gate ORs aura_runtime_multi_worker_production_latched.
    (
        "src/compiler/evaluator_security.cpp",
        r"const\s+bool\s+hard\s*=\s*typed_audit::should_hard_reject_soft_sibling\(\)\s*\|\|[\s\S]{0,80}aura_runtime_multi_worker_production_latched\(\)\s*!=\s*0",
        "3386 AC2: hard gate ORs aura_runtime_multi_worker_production_latched",
    ),
    # AC4: allow_query_stable_ref_export per-node eager-bit allow unchanged
    # (probe fix must not force hot-cone nodes red).
    (
        "src/compiler/evaluator_security.cpp",
        r"if\s*\(\s*ws->node_eagerly_restamped\(id\)\s*\)\s*return\s+true",
        "3386 AC4: allow_query_stable_ref_export preserves node-level eager-bit allow",
    ),
    # AC5: source-cite + cite #3386.
    (
        "src/compiler/evaluator_security.cpp",
        r"Issue\s+#3386",
        "3386 AC5: evaluator_security.cpp cites #3386",
    ),
)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _resolve(rel_path: str) -> Path:
    return REPO_ROOT / rel_path


def _check_pattern(rel_path: str, regex: str, *, strict: bool) -> list[str]:
    failures: list[str] = []
    p = _resolve(rel_path)
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    if not re.search(regex, text, re.MULTILINE) and strict:
        failures.append(f"{rel_path}: missing required pattern: {regex!r}")
    return failures


def _check_no_design_doc(strict: bool) -> list[str]:
    """AC5: no docs/design/3386-* markdown (per MEMORY 2026-07-19)."""
    failures: list[str] = []
    docs_dir = REPO_ROOT / "docs" / "design"
    if not docs_dir.exists():
        return failures
    matches = sorted(docs_dir.glob("3386-*.md"))
    if matches and strict:
        names = ", ".join(m.name for m in matches)
        failures.append(
            f"docs/design/3386-*.md exists ({names}); "
            "MEMORY 2026-07-19 forbids — close comment + commit carry rationale"
        )
    return failures


def _check_no_test_issue_file(strict: bool) -> list[str]:
    """AC5: no tests/issues/test_issue_3386.cpp (#81967)."""
    failures: list[str] = []
    p = REPO_ROOT / "tests" / "issues" / "test_issue_3386.cpp"
    if p.exists() and strict:
        failures.append(f"{p}: tests/issues/test_issue_3386.cpp exists; #81967 forbids — extend existing test")
    return failures


def run_checks(*, strict: bool) -> list[str]:
    failures: list[str] = []
    for rel, rx, _label in INFRA_REQUIRED:
        failures.extend(_check_pattern(rel, rx, strict=strict))
    failures.extend(_check_no_design_doc(strict))
    failures.extend(_check_no_test_issue_file(strict))
    return failures


def _self_test() -> int:
    """Validate the linter regexes against a fixture approximating the post-fix source."""
    fixture = """
    // Issue #3386 — probe predicate ORs restamp_over_budget_torn() under latch.
    bool Evaluator::query_stable_hard_reject_torn() const noexcept {
        const bool hard = typed_audit::should_hard_reject_soft_sibling() ||
                          aura::serve::aura_runtime_multi_worker_production_latched() != 0;
        if (!hard)
            return false;
        auto* ws = workspace_flat_;
        if (!ws)
            return false;
        return ws->restamp_last_budget_exceeded() || ws->nested_authority_gap() ||
               ws->restamp_over_budget_torn();
    }
    bool Evaluator::allow_query_stable_ref_export(ast::NodeId id) const noexcept {
        auto* ws = workspace_flat_;
        if (!ws || id == ast::NULL_NODE)
            return true;
        if (ws->node_eagerly_restamped(id))
            return true;
        if (!ws->nested_authority_gap() && !ws->restamp_last_budget_exceeded() &&
            !ws->restamp_over_budget_torn())
            return true;
        ...
    }
    """
    fails: list[str] = []
    for rel, rx, _label in INFRA_REQUIRED:
        if "evaluator_security.cpp" not in rel:
            continue
        if not re.search(rx, fixture, re.MULTILINE):
            fails.append(f"self-test: {rel}: missing pattern: {rx!r}")
    return 0 if not fails else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Issue #3386 query_stable_hard_reject_torn latch source-cite gate.")
    parser.add_argument("--strict", action="store_true", help="Fail on missing patterns (default: observe-only)")
    parser.add_argument("--self-test", action="store_true", help="Run self-test against fixture text")
    args = parser.parse_args(argv)
    if args.self_test:
        return _self_test()
    strict = bool(args.strict)
    failures = run_checks(strict=strict)
    if failures:
        print("Issue #3386 source-cite gate FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("Issue #3386 source-cite gate OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
