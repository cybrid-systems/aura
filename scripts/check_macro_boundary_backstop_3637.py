#!/usr/bin/env python3
# scripts/check_macro_boundary_backstop_3637.py -- Issue #3637 source-cite gate.
#
# Verifies the runtime boundary-level MacroIntroduced marker-delta backstop:
# the outermost MutationBoundaryGuard snapshots the cumulative macro-expansion
# dirty counter at enter, and at exit consults the SAME #1611
# MutationReflectHealth validator the eval_flat apply site uses. Production:
# dirty macro subtree without a recorded #3542 allow -> fail-closed rollback
# (+ hygiene_violation_prevented_on_boundary_total, #1908 counter). Soft/Off:
# end-append observe counter only, no rollback. Quiet path: one counter read
# per edge, no O(n) scan.
#
#  AC1: net fires under production for ANY path dirtying MacroIntroduced
#       without allow (adversarial enumeration-escape stub).
#  AC2: Soft/Off observe-only (end-append counter, no rollback); delta==0
#       quiet path = single counter read.
#  AC3: pre-gates stay the primary error surface; the net reuses the #1611
#       validator (existing error string; no new query keys; counter
#       appended at END, #2906).
#  AC4: the net never widens authorization — allow consults the #3542
#       recorded flag (get_allow_macro_mutate), never grants it.
#  AC5: test face in test_hygiene_mutate_closed_loop.cpp (production net,
#       Soft observe, allow pass) + regression suites
#       (test_hygiene_mutate_closed_loop / test_macro_hygiene_batch).
#  AC6: no docs/design/3637-* (#1655); no tests/**/test_issue_3637.cpp
#       (#81934); build.py wires this linter.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

EVAL_IXX = "src/compiler/evaluator.ixx"
MB = "src/compiler/evaluator_mutation_boundary.cpp"
METRICS = "src/compiler/observability_metrics.h"
TEST = "tests/compiler/test_hygiene_mutate_closed_loop.cpp"

REQUIRED: tuple[tuple[str, str, str], ...] = (
    (EVAL_IXX, r"macro_expansion_dirty_at_enter_", "3637 AC1: Guard snapshot member"),
    (MB, r"Issue\s+#3637", "3637 AC1: backstop cites #3637"),
    (MB, r"macro_expansion_dirty_at_enter_ =", "3637 AC1: ctor snapshot at enter"),
    (MB, r"macro_expansion_dirty_total\(\)", "3637 AC1: cumulative FlatAST counter read"),
    (
        MB,
        r"validate_mutation_reflect_health\(h3637, &backstop_err\)",
        "3637 AC3: hoisted #1611 validator (existing error string)",
    ),
    (MB, r"get_allow_macro_mutate\(\)", "3637 AC4: allow consults the #3542 recorded flag"),
    (MB, r"hygiene_violation_prevented_on_boundary_total", "3637 AC1: production reject bumps #1908 counter"),
    (
        MB,
        r"is_outermost_ && !cancel_forced_fail && ev_ && ev_->workspace_flat_",
        "3637 AC2: outermost-only + quiet-path guard",
    ),
    (METRICS, r"mutation_boundary_macro_hygiene_backstop_total", "3637 AC2: end-append observe counter"),
    (TEST, r"3637 AC1", "3637 AC5: test hosts production-net face"),
    (TEST, r"apply_macro_dirty_bits", "3637 AC5: adversarial enumeration-escape stub"),
    (TEST, r"apply_production_audit_defaults", "3637 AC5: production gate in test"),
    ("build.py", r"check_macro_boundary_backstop_3637", "3637 AC6: build.py wires the linter"),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def run_checks() -> list[str]:
    """Returns a list of failure labels (empty = clean)."""
    failures: list[str] = []
    cache: dict[str, str] = {}

    def body(path: str) -> str:
        if path not in cache:
            full = REPO_ROOT / path
            cache[path] = read(full) if full.exists() else ""
        return cache[path]

    for path, pattern, label in REQUIRED:
        if re.search(pattern, body(path)) is None:
            failures.append(label)

    mb = body(MB)

    # AC2 ordering: the backstop block must sit BEFORE the success fold so
    # the production forced-fail reaches exit_mutation_boundary(false).
    backstop_pos = mb.find("bool macro_hygiene_forced_fail = false;")
    fold_pos = mb.find("(cancel_forced_fail || macro_hygiene_forced_fail) ? false : success_flag_load(flag_);")
    if backstop_pos == -1 or fold_pos == -1:
        failures.append("3637 AC2: backstop / success-fold anchors not found")
    elif not (backstop_pos < fold_pos):
        failures.append("3637 AC2: backstop must precede the success fold")

    # AC2: the enter snapshot must sit inside the acquire path (after the
    # #2162 dirty-upward snapshot, same ctor).
    enter_pos = mb.find("macro_expansion_dirty_at_enter_ =")
    dup_pos = mb.find("dirty_upward_at_enter_ =")
    if enter_pos == -1 or dup_pos == -1 or enter_pos < dup_pos:
        failures.append("3637 AC1: ctor snapshot must follow the #2162 snapshot")

    # AC6: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3637-"):
                failures.append(f"3637 AC6: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3637.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3637.cpp",
    ):
        if probe.exists():
            failures.append(f"3637 AC6: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def self_test() -> int:
    """Regexes compile + anchor targets exist (pre-flight)."""
    ok = True
    for _, pattern, label in REQUIRED:
        try:
            re.compile(pattern)
        except re.error as e:
            print(f"SELF-TEST FAIL (regex compile): {label}: {e}")
            ok = False
    for path, _, _ in REQUIRED:
        if not (REPO_ROOT / path).exists():
            print(f"SELF-TEST FAIL (missing target): {path}")
            ok = False
    if ok:
        print("self-test: regexes compile + targets present")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3637 boundary macro-dirty backstop gate")
    ap.add_argument("--self-test", action="store_true", help="compile regexes + probe targets")
    ap.add_argument("--strict", action="store_true", help="exit non-zero on any failure")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    failures = run_checks()
    if failures:
        print(f"check_macro_boundary_backstop_3637: {len(failures)} failure(s)")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1 if args.strict else 0
    print("check_macro_boundary_backstop_3637: clean (outermost boundary marker-delta net)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
