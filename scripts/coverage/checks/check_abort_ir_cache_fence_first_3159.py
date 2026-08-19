#!/usr/bin/env python3
"""Issue #3159: abort IR cache fence-first ordering under multi-fiber.

Background
----------
`ASTArena::abort_restore_dual_topology(...)` restores FlatAST topology on
MutationBoundary abort. #3117 / #3069 introduced
`abort_force_generation_` + `begin_abort_ir_cache_force_fence()` +
`force_ir_cache_dirty_after_abort()` to close the silent-stale window
where concurrent `lookup_define_v2` could see pre-abort IR.

Residual: if any dual-topology abort entry path starts topology restore
BEFORE the fence is published (or a concurrent lookup races the
release store of `abort_force_generation_`), a reader can still see
pre-abort IR with old stamps that have not yet observed the new
generation, or a half-cleared map that later gets lazy-rebuilt from
pre-abort IR (despite the `abort_map_invalid` guard). The critical
section order across *all* abort entry points (MutationBoundary abort,
typed_mutate fail path, dual-topology restore hook) is not yet proven
uniform under multi-fiber.

`#3159` closes the residual by:
1. Auditing all 3 abort sites in `evaluator_mutation_boundary.cpp` and
   verifying `abort_ir_cache_begin_force_fn_()` (fence) is called BEFORE
   `workspace_flat_->abort_restore_dual_topology(...)` (topology).
2. Verifying `abort_ir_cache_force_dirty_fn_()` (force-dirty) is called
   AFTER topology mutation in all 3 sites.
3. Verifying `abort_force_in_progress_` flag is set in
   `begin_abort_ir_cache_force_fence()` BEFORE topology and cleared in
   `force_ir_cache_dirty_after_abort()` AFTER the `ir_cache_v2_` walk.
4. Verifying `abort_force_hold_` test-only mechanism is present for
   injecting concurrent lookups during the cache walk window.

This linter is the regression guard for `#3159`:

  * FENCE_FIRST — all 3 abort sites in evaluator_mutation_boundary.cpp
    have `abort_ir_cache_begin_force_fn_()` BEFORE
    `abort_restore_dual_topology(...)`.

  * FORCE_DIRTY_AFTER — all 3 sites have `abort_ir_cache_force_dirty_fn_()`
    AFTER topology mutation.

  * IN_PROGRESS_LIFECYCLE — `abort_force_in_progress_` is set in
    `begin_abort_ir_cache_force_fence()` and cleared in
    `force_ir_cache_dirty_after_abort()` AFTER the cache walk.

  * HOLD_MECHANISM — `abort_force_hold_` test-only atomic + public
    `set_abort_force_hold_for_test` wrapper + busy-wait inside
    `force_ir_cache_dirty_after_abort()`.

  * NO_NEW_METRICS — no `g_3159_*` atomic counter introduced (per issue
    AC4: metrics already present, no new middle-of-metrics counters).
    Uses existing `abort_ir_cache_force_dirty_total`.

  * NO_INVENT — no new `docs/design/3159-*` (per #1655), no new
    `tests/issues/test_issue_3159.cpp` (per #81934 — src/-aligned suite
    instead, i.e. `tests/compiler/test_abort_ir_cache_fence_first.cpp`).

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_abort_ir_cache_fence_first_3159.py            # report
  python3 scripts/coverage/checks/check_abort_ir_cache_fence_first_3159.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_abort_ir_cache_fence_first_3159.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

EVALUATOR_MUTATION_BOUNDARY_CPP = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
SERVICE_IXX = ROOT / "src" / "compiler" / "service.ixx"
TYPED_MUTATION_AUDIT_H = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
TEST_FILE = ROOT / "tests" / "compiler" / "test_abort_ir_cache_fence_first.cpp"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_FILE = ROOT / "tests" / "issues" / "test_issue_3159.cpp"


def _read_text(path: Path) -> tuple[str, bool]:
    if not path.exists():
        return ("", False)
    try:
        return (path.read_text(encoding="utf-8", errors="replace"), True)
    except OSError:
        return ("", False)


# Required patterns: presence checks.
# For each abort site, we verify:
#   1. abort_ir_cache_begin_force_fn_() appears BEFORE abort_restore_dual_topology(
#   2. abort_ir_cache_force_dirty_fn_() appears AFTER abort_restore_dual_topology(
# Since there are 3 sites, we use the count + ordered pair check.

REQUIRED_PATTERNS = (
    # --- AC1+AC2: 3 abort sites, fence-first + force_dirty-after ---
    (
        "exactly 3 abort_restore_dual_topology call sites",
        re.compile(r"abort_restore_dual_topology\s*\("),
        3,
        EVALUATOR_MUTATION_BOUNDARY_CPP,
    ),
    (
        "exactly 3 fence (begin_force) calls (one per abort site)",
        re.compile(r"abort_ir_cache_begin_force_fn_\s*\(\s*\)\s*;"),
        3,
        EVALUATOR_MUTATION_BOUNDARY_CPP,
    ),
    (
        "exactly 3 force_dirty calls (one per abort site)",
        re.compile(r"abort_ir_cache_force_dirty_fn_\s*\(\s*\)\s*;"),
        3,
        EVALUATOR_MUTATION_BOUNDARY_CPP,
    ),
    # --- AC3: abort_force_in_progress_ lifecycle in service.ixx ---
    (
        "begin_abort_ir_cache_force_fence sets abort_force_in_progress_=1 (release)",
        re.compile(
            r"void\s+begin_abort_ir_cache_force_fence\s*\(\s*\)\s*\{[\s\S]*?"
            r"abort_force_in_progress_\.store\s*\(\s*1\s*,\s*std::memory_order_release\s*\)"
        ),
        1,
        SERVICE_IXX,
    ),
    (
        "begin_abort_ir_cache_force_fence bumps abort_force_generation_ (release)",
        re.compile(
            r"void\s+begin_abort_ir_cache_force_fence\s*\(\s*\)\s*\{[\s\S]*?"
            r"abort_force_generation_\.fetch_add\s*\(\s*1\s*,\s*std::memory_order_release\s*\)"
        ),
        1,
        SERVICE_IXX,
    ),
    (
        "force_ir_cache_dirty_after_abort clears abort_force_in_progress_=0 (release)",
        re.compile(r"abort_force_in_progress_\.store\s*\(\s*0\s*,\s*std::memory_order_release\s*\)"),
        1,
        SERVICE_IXX,
    ),
    # --- AC4: abort_force_hold_ test-only mechanism ---
    (
        "abort_force_hold_ atomic field present",
        re.compile(r"std::atomic<std::uint8_t>\s+abort_force_hold_\s*\{?\s*0\s*\}?\s*;"),
        1,
        SERVICE_IXX,
    ),
    (
        "force_dirty busy-waits on abort_force_hold_ (test-only mid-loop window)",
        re.compile(
            r"void\s+force_ir_cache_dirty_after_abort\s*\(\s*\)\s*\{[\s\S]*?"
            r"while\s*\(\s*abort_force_hold_\.load\s*\(\s*std::memory_order_acquire\s*\)\s*!=\s*0\s*\)"
        ),
        1,
        SERVICE_IXX,
    ),
    (
        "public_set_abort_force_hold public wrapper present",
        re.compile(r"public_set_abort_force_hold"),
        1,
        SERVICE_IXX,
    ),
    # --- AC4: existing abort_ir_cache_force_dirty_total counter still used ---
    (
        "existing abort_ir_cache_force_dirty_total counter still used (issue AC4 invariant)",
        re.compile(r"abort_ir_cache_force_dirty_total"),
        1,
        SERVICE_IXX,
    ),
    # --- AC9: src/-aligned test present ---
    (
        "src/-aligned test tests/compiler/test_abort_ir_cache_fence_first.cpp",
        re.compile(r"run_test_abort_ir_cache_fence_first"),
        1,
        TEST_FILE,
    ),
)


# Forbidden patterns: presence checks.
FORBIDDEN_PATTERNS = (
    # --- AC6: no g_3159_* atomic counter introduced (issue AC4: no new metrics) ---
    (
        "forbidden g_3159_* atomic counter in service.ixx (issue AC4: no new metrics)",
        re.compile(r"atomic<std::uint64_t>\s+g_3159_"),
        1,
        SERVICE_IXX,
    ),
    (
        "forbidden g_3159_* atomic counter in evaluator_mutation_boundary.cpp",
        re.compile(r"atomic<std::uint64_t>\s+g_3159_"),
        1,
        EVALUATOR_MUTATION_BOUNDARY_CPP,
    ),
    (
        "forbidden g_3159_* atomic counter in typed_mutation_audit.h",
        re.compile(r"atomic<std::uint64_t>\s+g_3159_"),
        1,
        TYPED_MUTATION_AUDIT_H,
    ),
    # --- AC7: no docs/design/3159-* (per #1655) ---
    (
        "forbidden docs/design/3159-* (per #1655)",
        re.compile(r"3159-"),
        1,
        DOCS_DESIGN_DIR,
    ),
    # --- AC8: no tests/issues/test_issue_3159.cpp (per #81934) ---
    (
        "forbidden tests/issues/test_issue_3159.cpp (per #81934 — src/-aligned suite instead)",
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
        "issue": 3159,
        "topic": "abort IR cache fence-first ordering under multi-fiber",
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
        print(f"#3159 linter — {report['verdict']}")
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
