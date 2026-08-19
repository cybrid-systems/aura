#!/usr/bin/env python3
"""Issue #3158: occurrence abort restore-or-clear to entry authority.

Background
----------
`MutationBoundaryGuard` abort path (`evaluator_mutation_boundary.cpp`) clears
the TypeLinearCommitProof face (#3030) and truncates the CoercionMap cone
+ clears coercion readiness (#3102), but does NOT restore live
`ConstraintSystem::occurrence_goals_` (or related priority/fingerprint
state) back to the boundary-entry snapshot. Under production Full (high-freq
AI self-mutate loops that frequently abort — type-gate deny, linear synth-
hard-fail, composite empty-cs, cone-outside-drop reject), residual
narrowing goals poison the next delta's priority + fingerprint + stamp
face. The next outermost success may stamp on drifted CS truth.

`#3158` closes the residual under production/Full by capturing
`ConstraintSystem::occurrence_goals_size()` at boundary enter and
truncating back to that size on abort (after dual-topology restore + proof
+ coercion clear, before any post-abort IR/JIT lookup). Soft / Off bumps
an observe counter only (no structural write — zero-cost contract
preserved).

This linter is the regression guard for `#3158`:

  * ENTRY_CAPTURE — `MutationCheckpoint` struct has `occurrence_entry_size`
    field + captured at guard construction (reads
    `tc->constraint_system().occurrence_goals_size()`).

  * RESTORE_HELPER — `ConstraintSystem::restore_or_clear_occurrence_to_entry`
    exists and truncates via `occurrence_goals_.resize(entry_size)` with
    underflow guard returning 0.

  * ALL_3_ABORT_SITES — evaluator_mutation_boundary.cpp calls
    `restore_or_clear_occurrence_to_entry` from exactly 3 abort sites,
    paired with `note_3158_occurrence_abort_restore` (production/Full
    path) + `note_3158_occurrence_abort_observe` (Soft / Off path).

  * COUNTERS — typed_mutation_audit.h declares
    `g_3158_occurrence_abort_restore_total`,
    `g_3158_occurrence_abort_restore_goals_total`,
    `g_3158_occurrence_abort_observe_total` + `note_*` helpers +
    `occurrence_3158_abort_*_v_read` accessors.

  * NO_SECOND_MODEL — reuses live `occurrence_goals_` table (no parallel
    goal table per issue non-goals); existing `prune_occurrence_goals`
    + `clear_blame_context` + `maybe_persist_occurrence_snapshot`
    unchanged.

  * NO_INVENT — no new `docs/design/3158-*` (per #1655), no new
    `tests/issues/test_issue_3158.cpp` (per #81934 — src/-aligned suite
    instead, i.e. `tests/compiler/test_occurrence_abort_restore.cpp`).

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_occurrence_abort_restore_3158.py            # report
  python3 scripts/coverage/checks/check_occurrence_abort_restore_3158.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_occurrence_abort_restore_3158.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

EVALUATOR_IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
EVALUATOR_MUTATION_BOUNDARY_CPP = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
TYPE_CHECKER_IXX = ROOT / "src" / "compiler" / "type_checker.ixx"
TYPED_MUTATION_AUDIT_H = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
TEST_FILE = ROOT / "tests" / "compiler" / "test_occurrence_abort_restore.cpp"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_FILE = ROOT / "tests" / "issues" / "test_issue_3158.cpp"


def _read_text(path: Path) -> tuple[str, bool]:
    if not path.exists():
        return ("", False)
    try:
        return (path.read_text(encoding="utf-8", errors="replace"), True)
    except OSError:
        return ("", False)


# Required patterns: presence checks.
# Note on regex: function signatures in macro_expansion.cpp commonly have
# trailing qualifiers like `noexcept` between `)` and `{`. The `[^{]*\{`
# idiom allows arbitrary non-`{` text (including `noexcept`, comments, ws)
# before the body open, while still keeping the match tight (won't cross
# function boundaries since `{` only appears once per body).
_BODY_OPEN = r"[^{]*\{[\s\S]*?"  # once `{` is consumed, non-greedy to next anchor


def _count(text: str, pattern: str) -> int:
    return text.count(pattern)


REQUIRED_PATTERNS = (
    # --- AC1: MutationCheckpoint struct + entry capture ---
    (
        "MutationCheckpoint struct has occurrence_entry_size field",
        re.compile(r"std::size_t\s+occurrence_entry_size\s*=\s*0\s*;"),
        1,
        EVALUATOR_IXX,
    ),
    (
        "occurrence_entry_size captured at guard construction",
        re.compile(r"cp\.occurrence_entry_size\s*=\s*[^;]*occurrence_goals_size\s*\(\s*\)\s*;"),
        1,
        EVALUATOR_MUTATION_BOUNDARY_CPP,
    ),
    # --- AC2: restore_or_clear_occurrence_to_entry helper ---
    (
        "restore_or_clear_occurrence_to_entry helper declared",
        re.compile(r"restore_or_clear_occurrence_to_entry\s*\(\s*std::size_t\s+entry_size\s*\)"),
        1,
        TYPE_CHECKER_IXX,
    ),
    (
        "restore helper truncates via occurrence_goals_.resize(entry_size)",
        re.compile(r"occurrence_goals_\.resize\s*\(\s*entry_size\s*\)"),
        1,
        TYPE_CHECKER_IXX,
    ),
    (
        "restore helper underflow guard returns 0",
        re.compile(r"if\s*\(\s*live\s*<=\s*entry_size\s*\)\s*return\s+0\s*;"),
        1,
        TYPE_CHECKER_IXX,
    ),
    # --- AC3: 3 abort sites wired (presence checks; counts verified in test) ---
    (
        "restore_or_clear_occurrence_to_entry call present in boundary cpp",
        re.compile(r"restore_or_clear_occurrence_to_entry\s*\("),
        3,
        EVALUATOR_MUTATION_BOUNDARY_CPP,
    ),
    (
        "note_3158_occurrence_abort_restore call present in boundary cpp",
        re.compile(r"note_3158_occurrence_abort_restore\s*\("),
        3,
        EVALUATOR_MUTATION_BOUNDARY_CPP,
    ),
    (
        "note_3158_occurrence_abort_observe call present in boundary cpp",
        re.compile(r"note_3158_occurrence_abort_observe\s*\(\s*\)"),
        3,
        EVALUATOR_MUTATION_BOUNDARY_CPP,
    ),
    # --- AC4: new counters + helpers in typed_mutation_audit.h ---
    (
        "g_3158_occurrence_abort_restore_total counter declared",
        re.compile(r"std::atomic<std::uint64_t>\s+g_3158_occurrence_abort_restore_total"),
        1,
        TYPED_MUTATION_AUDIT_H,
    ),
    (
        "g_3158_occurrence_abort_restore_goals_total counter declared",
        re.compile(r"std::atomic<std::uint64_t>\s+g_3158_occurrence_abort_restore_goals_total"),
        1,
        TYPED_MUTATION_AUDIT_H,
    ),
    (
        "g_3158_occurrence_abort_observe_total counter declared",
        re.compile(r"std::atomic<std::uint64_t>\s+g_3158_occurrence_abort_observe_total"),
        1,
        TYPED_MUTATION_AUDIT_H,
    ),
    (
        "note_3158_occurrence_abort_restore helper present",
        re.compile(r"inline\s+void\s+note_3158_occurrence_abort_restore\s*\([^)]*goals_dropped[^)]*\)"),
        1,
        TYPED_MUTATION_AUDIT_H,
    ),
    (
        "note_3158_occurrence_abort_observe helper present",
        re.compile(r"inline\s+void\s+note_3158_occurrence_abort_observe\s*\(\s*\)"),
        1,
        TYPED_MUTATION_AUDIT_H,
    ),
    (
        "occurrence_3158_abort_restore_total_v_read accessor present",
        re.compile(r"occurrence_3158_abort_restore_total_v_read"),
        1,
        TYPED_MUTATION_AUDIT_H,
    ),
    (
        "occurrence_3158_abort_observe_total_v_read accessor present",
        re.compile(r"occurrence_3158_abort_observe_total_v_read"),
        1,
        TYPED_MUTATION_AUDIT_H,
    ),
    # --- AC5: no second model — reuses existing machinery ---
    (
        "prune_occurrence_goals still present (no duplicate model)",
        re.compile(r"prune_occurrence_goals"),
        1,
        TYPE_CHECKER_IXX,
    ),
    # --- AC6: src/-aligned test present ---
    (
        "src/-aligned test tests/compiler/test_occurrence_abort_restore.cpp",
        re.compile(r"run_test_occurrence_abort_restore"),
        1,
        TEST_FILE,
    ),
)


# Forbidden patterns: presence checks.
FORBIDDEN_PATTERNS = (
    # --- AC5: no g_3158_* atomic counter in type_checker.ixx (counters live in audit header) ---
    (
        "forbidden g_3158_* atomic in type_checker.ixx (counters in audit header)",
        re.compile(r"atomic<std::uint64_t>\s+g_3158_"),
        1,
        TYPE_CHECKER_IXX,
    ),
    (
        "forbidden parallel goal table marker in type_checker.ixx",
        re.compile(r"occurrence_goals_backup_|occurrence_goals_abort_"),
        1,
        TYPE_CHECKER_IXX,
    ),
    # --- AC6: no docs/design/3158-* (per #1655) ---
    (
        "forbidden docs/design/3158-* (per #1655)",
        re.compile(r"3158-"),
        1,
        DOCS_DESIGN_DIR,
    ),
    # --- AC7: no tests/issues/test_issue_3158.cpp (per #81934) ---
    (
        "forbidden tests/issues/test_issue_3158.cpp (per #81934 — src/-aligned suite instead)",
        re.compile(r"^."),
        1,
        ISSUES_TEST_FILE,
    ),
)


def collect_hits(patterns, root: Path) -> list[dict]:
    hits: list[dict] = []
    for label, regex, min_count, relpath in patterns:
        text, ok = _read_text(relpath)
        rel = str(relpath.relative_to(root)) if relpath.is_relative_to(root) else str(relpath)
        if not ok:
            hits.append(
                {
                    "label": label,
                    "path": rel,
                    "status": "MISSING",
                    "matches": 0,
                    "min": min_count,
                }
            )
            continue
        actual_count = len(regex.findall(text))
        if actual_count >= min_count:
            hits.append(
                {
                    "label": label,
                    "path": rel,
                    "status": "PASS",
                    "matches": actual_count,
                    "min": min_count,
                }
            )
        else:
            hits.append(
                {
                    "label": label,
                    "path": rel,
                    "status": "FAIL",
                    "matches": actual_count,
                    "min": min_count,
                }
            )
    return hits


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--strict", action="store_true", help="exit 1 on any required-missing or forbidden-present hit")
    parser.add_argument("--json", action="store_true", help="emit JSON report on stdout")
    parser.add_argument("--root", type=Path, default=ROOT, help="override repo root (default: script parent ^3)")
    args = parser.parse_args(argv)

    root = args.root.resolve()

    required_hits = collect_hits(REQUIRED_PATTERNS, root)
    forbidden_hits = collect_hits(FORBIDDEN_PATTERNS, root)

    required_missing = [h for h in required_hits if h["status"] != "PASS"]
    forbidden_present = [h for h in forbidden_hits if h["status"] == "PASS"]

    report = {
        "issue": 3158,
        "topic": "occurrence abort restore-or-clear to entry authority",
        "required_total": len(required_hits),
        "required_pass": len(required_hits) - len(required_missing),
        "required_missing": required_missing,
        "forbidden_total": len(forbidden_hits),
        "forbidden_pass": len(forbidden_hits) - len(forbidden_present),
        "forbidden_present": forbidden_present,
        "verdict": "clean" if not required_missing and not forbidden_present else "violation",
    }

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"#3158 linter — {report['verdict']}")
        print(f"  required: {report['required_pass']}/{report['required_total']} pass")
        for h in required_hits:
            mark = "✓" if h["status"] == "PASS" else "✗"
            tag = f" ({h['matches']}/{h['min']})" if h["min"] > 1 else ""
            print(f"    [{mark}] {h['label']}{tag}  ({h['path']})")
        print(f"  forbidden: {report['forbidden_pass']}/{report['forbidden_total']} absent")
        for h in forbidden_hits:
            mark = "✗ HIT" if h["status"] == "PASS" else "✓ absent"
            print(f"    [{mark}] {h['label']}  ({h['path']})")

    if args.strict and (required_missing or forbidden_present):
        return 1
    if required_missing or forbidden_present:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
