#!/usr/bin/env python3
"""Issue #3160: hold-budget inbody window escalate after 2× bound.

Background
----------
`aura_hold_budget_poll_inbody_window()` (fiber.cpp) is the production
watchdog for the in-body non-poll window after cancel+force-safepoint.
#3133 closed the primary synthetic-yield-inject side; #3133 is
set-and-forget — if the holder never reaches check_gc_safepoint /
yield / Phase-5, the lock + depth stay held past any multiple of the
hold SLO.

`#3160` closes the residual by:
1. Auditing the existing escalation path under the 2× bound contract
   (`mutation_hold_inbody_window_bound_us()` returns `slo * 2ULL` by
   default per #3071).
2. Verifying the one-shot `g_hold_budget_cancel_escalated.exchange(1, ...) == 0`
   pattern fires `aura_evaluator_force_degrade_outermost_holder(fid)`
   on first exceed (which does `request_cancel()` +
   `mark_outermost_mutation_failed()` — next edge cannot commit success).
3. Verifying later exceeds re-inject synthetic yield via
   `f->inject_synthetic_mutation_boundary_yield()` (keeps inject
   persistent until next cooperative edge).
4. Verifying Soft / sandbox=off path is metric-only (no escalate,
   no force_degrade).
5. Verifying no preemptive `workspace_mtx_` unlock while body is live
   (#3035 dual-topology contract preserved).

This linter is the regression guard for `#3160`:

  * TWO_X_BOUND — `mutation_hold_inbody_window_bound_us()` returns
    `slo * 2ULL` (default 2× hold SLO per #3071); poll escalates when
    `elapsed_us > bound_us`.

  * ONE_SHOT_ESCALATE — escalate is one-shot per arm via
    `g_hold_budget_cancel_escalated.exchange(1, std::memory_order_acq_rel) == 0`.

  * FORCE_DEGRADE — first escalate calls
    `aura_evaluator_force_degrade_outermost_holder(fid)` which does
    `request_cancel()` + `mark_outermost_mutation_failed()`.

  * RE_INJECT — later exceeds call
    `f->inject_synthetic_mutation_boundary_yield()` (keeps inject
    persistent until next cooperative edge).

  * SOFT_METRIC_ONLY — Soft / sandbox=off path returns 0 (metric-only,
    no escalate / no force_degrade) via
    `mutation_hold_budget_reject_enabled()` gate.

  * NO_PREEMPTIVE_UNLOCK — poll does NOT touch `workspace_mtx_` or any
    mutex unlock (#3035 dual-topology contract preserved).

  * NO_NEW_METRICS — no `g_3160_*` atomic counter introduced (uses
    existing `g_mutation_hold_budget_inbody_window_exceeded_total` +
    `g_mutation_hold_budget_forced_fail_closed_total` +
    `g_hold_budget_cancel_escalated`).

  * EXTENDS_EXISTING_SUITE — extends existing
    `test_hold_budget_synthetic_yield_injection` suite (#81967); no
    new `tests/issues/test_issue_3160.cpp`.

  * NO_INVENT — no new `docs/design/3160-*` (per #1655).

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_hold_budget_inbody_escalate_3160.py            # report
  python3 scripts/coverage/checks/check_hold_budget_inbody_escalate_3160.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_hold_budget_inbody_escalate_3160.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

FIBER_CPP = ROOT / "src" / "serve" / "fiber.cpp"
EVALUATOR_FIBER_MUTATION_CPP = ROOT / "src" / "compiler" / "evaluator_fiber_mutation.cpp"
MUTATION_HOLD_BUDGET_H = ROOT / "src" / "compiler" / "mutation_hold_budget.h"
TEST_FILE = ROOT / "tests" / "serve" / "test_hold_budget_synthetic_yield_injection.cpp"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_FILE = ROOT / "tests" / "issues" / "test_issue_3160.cpp"


def _read_text(path: Path) -> tuple[str, bool]:
    if not path.exists():
        return ("", False)
    try:
        return (path.read_text(encoding="utf-8", errors="replace"), True)
    except OSError:
        return ("", False)


# Required patterns: presence checks.
REQUIRED_PATTERNS = (
    # --- AC6: 2× bound threshold ---
    (
        "poll reads bound from mutation_hold_inbody_window_bound_us()",
        re.compile(r"const\s+auto\s+bound_us\s*=\s*mutation_hold_inbody_window_bound_us\s*\(\s*\)\s*;"),
        1,
        FIBER_CPP,
    ),
    (
        "poll escalates when elapsed_us > bound_us",
        re.compile(r"if\s*\(\s*elapsed_us\s*<=\s*bound_us\s*\)\s*return\s+0\s*;"),
        1,
        FIBER_CPP,
    ),
    (
        "bound default = slo * 2ULL (2× hold SLO per #3071)",
        re.compile(r"return\s+slo\s*\*\s*2ULL\s*;"),
        1,
        MUTATION_HOLD_BUDGET_H,
    ),
    # --- AC7: one-shot escalate via exchange(1) ---
    (
        "escalate is one-shot via exchange(1, acq_rel) == 0",
        re.compile(r"g_hold_budget_cancel_escalated\.exchange\s*\(\s*1\s*,\s*std::memory_order_acq_rel\s*\)\s*==\s*0"),
        1,
        FIBER_CPP,
    ),
    # --- AC8: first escalate calls force_degrade ---
    (
        "first escalate calls aura_evaluator_force_degrade_outermost_holder(fid)",
        re.compile(r"aura_evaluator_force_degrade_outermost_holder\s*\(\s*fid\s*\)"),
        1,
        FIBER_CPP,
    ),
    (
        "force_degrade_outermost_holder calls request_cancel()",
        re.compile(
            r"aura_evaluator_force_degrade_outermost_holder\s*\(\s*std::uint64_t\s+fiber_id\s*\)"
            r"[\s\S]*?request_cancel\s*\(\s*\)"
        ),
        1,
        EVALUATOR_FIBER_MUTATION_CPP,
    ),
    (
        "force_degrade_outermost_holder calls mark_outermost_mutation_failed()",
        re.compile(
            r"aura_evaluator_force_degrade_outermost_holder\s*\(\s*std::uint64_t\s+fiber_id\s*\)"
            r"[\s\S]*?mark_outermost_mutation_failed\s*\(\s*\)"
        ),
        1,
        EVALUATOR_FIBER_MUTATION_CPP,
    ),
    # --- AC9: later exceeds re-inject synthetic yield ---
    (
        "later exceeds re-inject synthetic yield",
        re.compile(r"f->inject_synthetic_mutation_boundary_yield\s*\(\s*\)\s*;"),
        1,
        FIBER_CPP,
    ),
    # --- AC11: Soft / sandbox=off metric-only ---
    (
        "Soft gate via mutation_hold_budget_reject_enabled()",
        re.compile(r"if\s*\(\s*!mutation_hold_budget_reject_enabled\s*\(\s*\)\s*\)"),
        1,
        FIBER_CPP,
    ),
    (
        "Soft returns 0 (metric-only)",
        re.compile(r"return\s+0\s*;\s*//\s*Soft\s*/\s*sandbox=off"),
        1,
        FIBER_CPP,
    ),
    # --- AC13: existing counters preserved ---
    (
        "existing #3035 forced_fail_closed_total preserved",
        re.compile(r"g_mutation_hold_budget_forced_fail_closed_total"),
        1,
        MUTATION_HOLD_BUDGET_H,
    ),
    (
        "existing #3071 inbody_window_exceeded_total preserved",
        re.compile(r"g_mutation_hold_budget_inbody_window_exceeded_total"),
        1,
        MUTATION_HOLD_BUDGET_H,
    ),
    (
        "existing #3071 cancel_escalated flag preserved",
        re.compile(r"g_hold_budget_cancel_escalated"),
        1,
        MUTATION_HOLD_BUDGET_H,
    ),
    # --- AC14: extends existing test suite ---
    (
        "extends existing test_hold_budget_synthetic_yield_injection suite",
        re.compile(r"run_test_hold_budget_inbody_escalate"),
        1,
        TEST_FILE,
    ),
    # --- AC15: no invent docs ---
    (
        "src/-aligned test extensions present (run_test_hold_budget_inbody_escalate)",
        re.compile(r"run_test_hold_budget_inbody_escalate"),
        1,
        TEST_FILE,
    ),
)


# Forbidden patterns: presence checks.
FORBIDDEN_PATTERNS = (
    # --- AC12: no preemptive workspace_mtx_ unlock while body is live ---
    # Look for actual unlock/lock calls on workspace_mtx_ (not just
    # comments mentioning the #3035 dual-topology contract).
    (
        "forbidden workspace_mtx_.unlock() call in poll (#3035 dual-topology contract)",
        re.compile(r"workspace_mtx_\.unlock\s*\("),
        1,
        FIBER_CPP,
    ),
    (
        "forbidden workspace_mtx_.lock() call in poll (poll is read-only / polling-only)",
        re.compile(r"workspace_mtx_\.lock\s*\("),
        1,
        FIBER_CPP,
    ),
    # --- AC13: no new middle-of-metrics counter ---
    (
        "forbidden g_3160_* atomic counter in fiber.cpp (issue AC4: reuse existing)",
        re.compile(r"atomic<std::uint64_t>\s+g_3160_"),
        1,
        FIBER_CPP,
    ),
    (
        "forbidden g_3160_* atomic counter in mutation_hold_budget.h",
        re.compile(r"atomic<std::uint64_t>\s+g_3160_"),
        1,
        MUTATION_HOLD_BUDGET_H,
    ),
    # --- AC15: no docs/design/3160-* (per #1655) ---
    (
        "forbidden docs/design/3160-* (per #1655)",
        re.compile(r"3160-"),
        1,
        DOCS_DESIGN_DIR,
    ),
    # --- AC14: no tests/issues/test_issue_3160.cpp (per #81934) ---
    (
        "forbidden tests/issues/test_issue_3160.cpp (per #81934 — extends existing suite instead)",
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
        "issue": 3160,
        "topic": "hold-budget inbody window escalate after 2× bound",
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
        print(f"#3160 linter — {report['verdict']}")
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
