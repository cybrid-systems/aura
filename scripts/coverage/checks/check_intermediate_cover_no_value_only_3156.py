#!/usr/bin/env python3
"""Issue #3156: close value-only dual-track under production required.

Background
----------
`ASTArena::maybe_note_allocate_intermediate_(void*, size_t)` (#3053) gates on
`general_object_pin_required_active()` and calls
`note_intermediate_create_auto_wire_(ptr)` — a value-only prep that bumps
`g_intermediate_create_value_only_total` (observability only per #3017, NOT
safe cover). `#3093` introduced `note_intermediate_create_with_cover_` but the
both-null fallback STILL calls `note_intermediate_create_auto_wire_(p)`, and
the allocate path kept the direct auto_wire_ call.

This linter is the regression guard for `#3156`:

  * NO_DIRECT_AUTO_WIRE — `maybe_note_allocate_intermediate_` body MUST NOT
    call `note_intermediate_create_auto_wire_(ptr)` directly. The dual-track
    must route through `note_intermediate_create_with_cover_` so the
    required+both-null branch (which fail-closes via
    intermediate_creates_ + new uncovered metric, NOT value_only) takes over.

  * WITH_COVER_DUAL_TRACK_CLOSED — `note_intermediate_create_with_cover_`
    body MUST reference `general_object_pin_required_active()` for the
    both-null branch, and the auto_wire_ call (if present) MUST sit AFTER
    that required_active check (i.e. Soft/Off/render-hotpath fallback only).

  * COUNTER_PRESENT — `g_intermediate_create_uncovered_under_required_total`
    counter declaration + `intermediate_create_uncovered_under_required_total_v_read`
    accessor + `kIntermediateCreateUncoveredUnderRequiredIssue = 3156` stamp.

  * RESET_HELPER_3_COUNTERS — `reset_intermediate_create_with_cover_for_test`
    resets all 3 counters (with_cover / value_only / uncovered_under_required).

  * NO_INVENT — no new `docs/design/3156-*` (per #1655), no new
    `tests/issues/test_issue_3156.cpp` (per #81934 — src/-aligned suite
    instead, i.e. `tests/core/test_arena_required_cover_no_value_only.cpp`).

  * SOFT_ZERO_COST — `maybe_note_allocate_intermediate_` keeps the
    required_active single load + in_render_hotpath gate + kMaxSmallSize /
    small_pool_.owns checks unchanged (AC3 zero-cost contract).

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py            # report
  python3 scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

ARENA_IXX = ROOT / "src" / "core" / "arena.ixx"
TEST_DIR = ROOT / "tests" / "core" / "test_arena_required_cover_no_value_only.cpp"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3156.cpp"


# Required patterns: presence checks.
# Note on regex: function signatures in arena.ixx commonly have trailing
# qualifiers like `noexcept` between `)` and `{`. The `[^{]*\{` idiom
# allows arbitrary non-`{` text (including `noexcept`, comments, ws) before
# the body open, while still keeping the match tight (won't cross function
# boundaries since `{` only appears once per body).
_REQUIRED_BODY = r"[^{]*\{[\s\S]*?"  # once the `{` is consumed, non-greedy to next anchor

REQUIRED_PATTERNS = (
    # --- AC1: maybe_note_allocate_intermediate_ body must NOT call auto_wire_ directly ---
    # We check this via the inverse: the function body should call with_cover_ instead.
    (
        "maybe_note_allocate_intermediate_ routes through with_cover_",
        re.compile(
            r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)"
            + _REQUIRED_BODY
            + r"note_intermediate_create_with_cover_\s*\(\s*ptr"
        ),
        1,
        ARENA_IXX,
    ),
    # --- AC3: with_cover_ body references general_object_pin_required_active ---
    (
        "with_cover_ body references general_object_pin_required_active",
        re.compile(
            r"void\s+note_intermediate_create_with_cover_\s*\([^)]*\)"
            + _REQUIRED_BODY
            + r"general_object_pin_required_active\s*\("
        ),
        1,
        ARENA_IXX,
    ),
    # --- AC4: with_cover_ body bumps the new uncovered metric ---
    (
        "with_cover_ body bumps g_intermediate_create_uncovered_under_required_total",
        re.compile(
            r"void\s+note_intermediate_create_with_cover_\s*\([^)]*\)"
            + _REQUIRED_BODY
            + r"g_intermediate_create_uncovered_under_required_total\.fetch_add"
        ),
        1,
        ARENA_IXX,
    ),
    # --- AC4: with_cover_ body still inventories into intermediate_creates_ ---
    (
        "with_cover_ body inventories intermediate_creates_.push_back(p)",
        re.compile(
            r"void\s+note_intermediate_create_with_cover_\s*\([^)]*\)"
            + _REQUIRED_BODY
            + r"intermediate_creates_\.push_back\s*\(\s*p\s*\)"
        ),
        1,
        ARENA_IXX,
    ),
    # --- AC5: new counter declared ---
    (
        "g_intermediate_create_uncovered_under_required_total counter",
        re.compile(r"std::atomic<std::uint64_t>\s+g_intermediate_create_uncovered_under_required_total"),
        1,
        ARENA_IXX,
    ),
    # --- AC5: stamp constant ---
    (
        "kIntermediateCreateUncoveredUnderRequiredIssue = 3156 stamp",
        re.compile(r"kIntermediateCreateUncoveredUnderRequiredIssue\s*=\s*3156"),
        1,
        ARENA_IXX,
    ),
    # --- AC6: accessor ---
    (
        "intermediate_create_uncovered_under_required_total_v_read accessor",
        re.compile(r"intermediate_create_uncovered_under_required_total_v_read\s*\("),
        1,
        ARENA_IXX,
    ),
    # --- AC7: reset helper resets all 3 counters ---
    (
        "reset_intermediate_create_with_cover_for_test resets all 3 counters",
        re.compile(
            r"void\s+reset_intermediate_create_with_cover_for_test\s*\([^)]*\)"
            + _REQUIRED_BODY
            + r"g_intermediate_create_with_cover_total\.store\(0[\s\S]*?"
            + r"g_intermediate_create_value_only_total\.store\(0[\s\S]*?"
            + r"g_intermediate_create_uncovered_under_required_total\.store\(0"
        ),
        1,
        ARENA_IXX,
    ),
    # --- AC8: maybe_note_allocate_intermediate_ keeps single required load + render gate ---
    (
        "maybe_note_allocate_intermediate_ keeps single required_active load",
        re.compile(
            r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)"
            + _REQUIRED_BODY
            + r"general_object_pin_required_active\s*\(\)"
        ),
        1,
        ARENA_IXX,
    ),
    (
        "maybe_note_allocate_intermediate_ keeps in_render_hotpath gate",
        re.compile(
            r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)" + _REQUIRED_BODY + r"in_render_hotpath\s*\(\)"
        ),
        1,
        ARENA_IXX,
    ),
    # --- AC9: src/-aligned test present ---
    (
        "src/-aligned test tests/core/test_arena_required_cover_no_value_only.cpp",
        re.compile(r"run_test_arena_required_cover_no_value_only"),
        1,
        TEST_DIR,
    ),
)


# Forbidden patterns: presence checks.
FORBIDDEN_PATTERNS = (
    # --- AC1: maybe_note_allocate_intermediate_ must NOT call auto_wire_ directly ---
    (
        "maybe_note_allocate_intermediate_ calls note_intermediate_create_auto_wire_(ptr) directly",
        re.compile(
            r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)"
            + _REQUIRED_BODY
            + r"note_intermediate_create_auto_wire_\s*\(\s*ptr\s*\)"
        ),
        1,
        ARENA_IXX,
    ),
    # --- AC4: with_cover_ required branch must NOT call auto_wire_ — the auto_wire_
    # call, if present, MUST sit AFTER general_object_pin_required_active()
    # (Soft/Off/render-hotpath fallback). We enforce this by requiring the
    # auto_wire_ call position to be after the required_active reference.
    # We model this as a forbidden-pattern heuristic: any auto_wire_ call
    # appearing BEFORE a required_active call in the with_cover_ body is
    # forbidden (would re-introduce the dual-track).
    (
        "with_cover_ has auto_wire_ call BEFORE required_active check (dual-track)",
        re.compile(
            r"void\s+note_intermediate_create_with_cover_\s*\([^)]*\)" + r"[^{]*\{"
            # capture everything up to the first auto_wire_ call (without
            # crossing a required_active call)
            r"(?:(?!general_object_pin_required_active).)*?"
            r"note_intermediate_create_auto_wire_\s*\(\s*p\s*\)"
        ),
        1,
        ARENA_IXX,
    ),
    # --- AC10: no docs/design/3156-* (per #1655) ---
    (
        "forbidden docs/design/3156-* (per #1655)",
        re.compile(r"3156-"),
        1,
        DOCS_DESIGN_DIR,
    ),
    # --- AC10: no tests/issues/test_issue_3156.cpp (per #81934) ---
    (
        "forbidden tests/issues/test_issue_3156.cpp (per #81934 — src/-aligned suite instead)",
        re.compile(r"^."),
        1,
        ISSUES_TEST_DIR,
    ),
)


def collect_hits(patterns, root: Path) -> list[dict]:
    hits: list[dict] = []
    for label, regex, _min, relpath in patterns:
        path = relpath
        if not path.exists():
            hits.append(
                {
                    "label": label,
                    "path": str(path.relative_to(root)) if path.is_relative_to(root) else str(path),
                    "status": "MISSING",
                    "matches": 0,
                }
            )
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            hits.append(
                {
                    "label": label,
                    "path": str(path.relative_to(root)) if path.is_relative_to(root) else str(path),
                    "status": f"READ_ERROR: {exc}",
                    "matches": 0,
                }
            )
            continue
        count = len(regex.findall(text))
        if count >= 1:
            hits.append(
                {
                    "label": label,
                    "path": str(path.relative_to(root)) if path.is_relative_to(root) else str(path),
                    "status": "PASS",
                    "matches": count,
                }
            )
        else:
            hits.append(
                {
                    "label": label,
                    "path": str(path.relative_to(root)) if path.is_relative_to(root) else str(path),
                    "status": "FAIL",
                    "matches": 0,
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
        "issue": 3156,
        "topic": "close value-only dual-track under production required",
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
        print(f"#3156 linter — {report['verdict']}")
        print(f"  required: {report['required_pass']}/{report['required_total']} pass")
        for h in required_hits:
            mark = "✓" if h["status"] == "PASS" else "✗"
            print(f"    [{mark}] {h['label']}  ({h['path']}, {h['matches']} match(es))")
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
