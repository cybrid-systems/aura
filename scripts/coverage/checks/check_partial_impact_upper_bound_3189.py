#!/usr/bin/env python3
"""Issue #3189: unify impact upper-bound on every production partial-relower decision site (fail-closed).

Closes the residual where `invalidate_bridge_with_impact` (the quote / lambda path inside `invalidate_function`) chose partial based on `scope.affected_instrs/blocks.size() < threshold` without consulting the impact-checked helper that catches cross-fn callee under-count. Existing siblings (#3034 `try_partial_invalidate_relower` + #2246 `apply_partial_relower_storm_gate`) already call the helper — #3189 unifies the third site.

Contract (one row per AC):
  AC1  every production partial decision site calls
       should_partial_relower_impact_checked(dirty_n, impact_ub):
         - try_partial_invalidate_relower (service_dirty.cpp:1270, #3034)
         - apply_partial_relower_storm_gate (service.ixx:7021, #2246)
         - invalidate_bridge_with_impact (service_dirty.cpp:1163, #3189 NEW)
  AC2  invalidate_bridge_with_impact source-cites the dirty_count_est vs
       impact_ub decision; bumps partial_forced_full_by_impact_total on
       under-estimate (existing counter, no new metric key)
  AC3  compute_impact_scope precedes the impact-checked helper in
       invalidate_bridge_with_impact so cross-fn callee scope is available
       for impact_ub
  AC4  Soft path unchanged — empty scope early-exits; helper has Soft / clean
       early-return (zero extra work on quiet windows)
  AC5  existing #2560 / #2246 / #3034 sibling ACs preserved
  AC6  tests/compiler/test_partial_cone_cap.cpp extended with
       ac3189_partial_impact_upper_bound_unified source-cite ACs;
       linter wired in build.py after #3188; no docs/design/3189-*
       (#1655); no tests/issues/test_issue_3189.cpp (#81934)
  AC7  (reserved for self-test / future runtime AC)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_partial_impact_upper_bound_3189.py            # report
  python3 scripts/coverage/checks/check_partial_impact_upper_bound_3189.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_partial_impact_upper_bound_3189.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SERVICE_DIRTY = ROOT / "src" / "compiler" / "service_dirty.cpp"
SERVICE_IXX = ROOT / "src" / "compiler" / "service.ixx"
IR_CACHE_PURE = ROOT / "src" / "compiler" / "ir_cache_pure.ixx"
OBS_METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
TEST_PARTIAL_CONE = ROOT / "tests" / "compiler" / "test_partial_cone_cap.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3189.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def _function_body_window(haystack: str, signature: str, next_signature_prefix: str = "void CompilerService::") -> str:
    """Return the body of the function identified by signature, bounded by the next function start.

    More robust than column-0 brace detection (which can be confused by nested
    column-0 patterns). Searches for the next `next_signature_prefix` after the
    given signature and returns the window up to that point.
    """
    pos = haystack.find(signature)
    if pos == -1:
        return ""
    end_search = haystack.find("\n" + next_signature_prefix, pos + 1)
    if end_search == -1:
        end_search = pos + 50000
    return haystack[pos:end_search]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3189 unify impact upper-bound on every production partial-relower decision site"
    )
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    svc = _read(SERVICE_DIRTY)
    sixx = _read(SERVICE_IXX)
    ixx = _read(IR_CACHE_PURE)
    obs = _read(OBS_METRICS)
    test = _read(TEST_PARTIAL_CONE)
    build = _read(BUILD_PY)

    # ── AC1: every production partial decision site calls the helper ──
    ac1_try_partial = "should_partial_relower_impact_checked(dirty_n, impact_ub)" in svc
    # Issue #3310: apply_partial_relower_storm_gate partial branch
    # may now route through should_partial_relower_impact_checked_prod
    # (which delegates to should_partial_relower_impact_checked). The
    # AC1 contract is "every production partial decision site calls the
    # helper" — the delegating helper satisfies that contract, so
    # accept either form here.
    ac1_apply_partial = (
        "should_partial_relower_impact_checked(dirty_n, impact_ub)" in sixx
        or "should_partial_relower_impact_checked_prod(dirty_n, impact_ub," in sixx
    )
    ac1_invalidate_bridge = "should_partial_relower_impact_checked(dirty_count_est, impact_ub)" in svc
    ac1_helper_defined = "should_partial_relower_impact_checked" in ixx
    ac1_ok = ac1_try_partial and ac1_apply_partial and ac1_invalidate_bridge and ac1_helper_defined
    if not ac1_helper_defined:
        fails.append("AC1: helper should_partial_relower_impact_checked missing in ir_cache_pure.ixx")
    if not ac1_try_partial:
        fails.append(
            "AC1: try_partial_invalidate_relower (service_dirty.cpp) must call helper with (dirty_n, impact_ub)"
        )
    if not ac1_apply_partial:
        fails.append("AC1: apply_partial_relower_storm_gate (service.ixx) must call helper with (dirty_n, impact_ub)")
    if not ac1_invalidate_bridge:
        fails.append(
            "AC1: invalidate_bridge_with_impact (service_dirty.cpp) must call helper with (dirty_count_est, impact_ub) — #3189 NEW"
        )
    rows.append(
        {
            "ac": "AC1_helper_called_at_every_production_partial_decision_site",
            "ok": ac1_ok,
            "try_partial_invalidate_relower": ac1_try_partial,
            "apply_partial_relower_storm_gate": ac1_apply_partial,
            "invalidate_bridge_with_impact": ac1_invalidate_bridge,
            "helper_defined": ac1_helper_defined,
        }
    )

    # ── AC2: source-cite the dirty_count_est vs impact_ub decision + metric bump ──
    ac2_cite = "Issue #3189 AC1: every production partial decision entry" in svc
    ac2_dirty_est = "dirty_count_est" in svc
    ac2_impact_ub = "impact_upper_bound_for_entry_(affected_name, cit->second)" in svc
    ac2_metric_bump = "partial_forced_full_by_impact_total.fetch_add(" in svc
    ac2_metric_defined = "partial_forced_full_by_impact_total" in obs
    ac2_ok = ac2_cite and ac2_dirty_est and ac2_impact_ub and ac2_metric_bump and ac2_metric_defined
    if not ac2_cite:
        fails.append(
            "AC2: invalidate_bridge_with_impact must cite 'Issue #3189 AC1: every production partial decision entry'"
        )
    if not ac2_dirty_est:
        fails.append("AC2: dirty_count_est variable must be defined (scope.affected_blocks + scope.affected_instrs)")
    if not ac2_impact_ub:
        fails.append("AC2: impact_ub must be computed via impact_upper_bound_for_entry_(affected_name, cit->second)")
    if not ac2_metric_bump:
        fails.append("AC2: partial_forced_full_by_impact_total.fetch_add must be called on under-estimate")
    if not ac2_metric_defined:
        fails.append(
            "AC2: existing counter partial_forced_full_by_impact_total must be defined in observability_metrics.h (no new metric key)"
        )
    rows.append(
        {
            "ac": "AC2_source_cite_dirty_count_impact_ub_metric_bump",
            "ok": ac2_ok,
            "cite": ac2_cite,
            "dirty_count_est": ac2_dirty_est,
            "impact_ub_compute": ac2_impact_ub,
            "metric_bump": ac2_metric_bump,
            "metric_reused": ac2_metric_defined,
        }
    )

    # ── AC3: compute_impact_scope precedes the impact-checked helper ──
    ib_window = _function_body_window(svc, "invalidate_bridge_with_impact")
    ac3_scope_call = "compute_impact_scope" in ib_window
    ac3_helper_call = "should_partial_relower_impact_checked" in ib_window
    ac3_scope_before_helper = False
    if ac3_scope_call and ac3_helper_call:
        scope_pos = ib_window.find("compute_impact_scope")
        helper_pos = ib_window.find("should_partial_relower_impact_checked")
        ac3_scope_before_helper = scope_pos < helper_pos
    ac3_ok = ac3_scope_call and ac3_helper_call and ac3_scope_before_helper
    if not ib_window:
        fails.append("AC3: invalidate_bridge_with_impact function not found in service_dirty.cpp")
    if not ac3_scope_call:
        fails.append("AC3: invalidate_bridge_with_impact must call compute_impact_scope (cross-fn callee scope)")
    if not ac3_helper_call:
        fails.append("AC3: invalidate_bridge_with_impact must call the impact-checked helper")
    if not ac3_scope_before_helper:
        fails.append(
            "AC3: compute_impact_scope must precede the impact-checked helper (cross-fn scope must be available for impact_ub)"
        )
    rows.append(
        {
            "ac": "AC3_compute_impact_scope_precedes_helper",
            "ok": ac3_ok,
            "compute_impact_scope_call": ac3_scope_call,
            "helper_call": ac3_helper_call,
            "scope_before_helper": ac3_scope_before_helper,
        }
    )

    # ── AC4: Soft path unchanged — empty scope early-exits ──
    helper_def_pos = ixx.find("should_partial_relower_impact_checked")
    helper_body = ""
    if helper_def_pos != -1:
        helper_end = ixx.find("\n}\n", helper_def_pos)
        if helper_end != -1:
            helper_body = ixx[helper_def_pos:helper_end]
    # The helper returns false (no partial) when dirty_count == 0 (clean —
    # nothing to do, zero-cost early exit). The check for
    # `impact_upper_bound > dirty_count` forces full when impact exceeds
    # local mask. Together they preserve Soft / Off zero-cost under
    # production_defaults_active().
    ac4_clean_early_return = "dirty_count == 0" in helper_body
    ac4_impact_exceeds_check = "impact_upper_bound > dirty_count" in helper_body
    # Also verify Soft path in invalidate_bridge_with_impact still has
    # the empty-scope early-exit guard.
    ac4_empty_guard = "scope.affected_blocks.empty()" in ib_window and "scope.affected_instrs.empty()" in ib_window
    ac4_ok = ac4_clean_early_return and ac4_impact_exceeds_check and ac4_empty_guard
    if not ac4_clean_early_return:
        fails.append("AC4: helper must have clean-window early-return when dirty_count == 0")
    if not ac4_impact_exceeds_check:
        fails.append("AC4: helper must force full when impact_upper_bound > dirty_count")
    if not ac4_empty_guard:
        fails.append("AC4: invalidate_bridge_with_impact must still guard on empty scope (Soft zero-cost contract)")
    rows.append(
        {
            "ac": "AC4_soft_zero_cost_unchanged",
            "ok": ac4_ok,
            "helper_clean_early_return": ac4_clean_early_return,
            "helper_impact_exceeds_check": ac4_impact_exceeds_check,
            "empty_scope_guard": ac4_empty_guard,
        }
    )

    # ── AC5: existing #2560 / #2246 / #3034 sibling ACs preserved ──
    # #2560 sibling surface (partial_cone*) lives in typed_mutation_audit.h
    # (cone cap stats are process-atomic). Check there, not in service_dirty.cpp.
    typed_audit = _read(ROOT / "src" / "compiler" / "typed_mutation_audit.h")
    ac5_2560_cone = "partial_cone" in typed_audit
    ac5_3034_try_partial = "try_partial_invalidate_relower" in svc
    ac5_2246_apply_partial = "apply_partial_relower_storm_gate" in sixx
    ac5_ok = ac5_2560_cone and ac5_3034_try_partial and ac5_2246_apply_partial
    if not ac5_2560_cone:
        fails.append("AC5: #2560 partial-cone sibling surface (partial_cone*) must remain in typed_mutation_audit.h")
    if not ac5_3034_try_partial:
        fails.append("AC5: #3034 try_partial_invalidate_relower sibling surface must remain in service_dirty.cpp")
    if not ac5_2246_apply_partial:
        fails.append("AC5: #2246 apply_partial_relower_storm_gate sibling surface must remain in service.ixx")
    rows.append(
        {
            "ac": "AC5_sibling_invariants_preserved",
            "ok": ac5_ok,
            "2560_cone_cap": ac5_2560_cone,
            "3034_try_partial": ac5_3034_try_partial,
            "2246_apply_partial": ac5_2246_apply_partial,
        }
    )

    # ── AC6: test extension + build.py wire-in + no docs/design/ + no tests/issues/ ──
    ac6_test_extended = "ac3189_partial_impact_upper_bound_unified" in test
    ac6_ac1 = "ac3189 AC1:" in test
    ac6_ac2 = "ac3189 AC2:" in test
    ac6_ac3 = "ac3189 AC3:" in test
    ac6_ac4 = "ac3189 AC4:" in test
    ac6_ac5 = "ac3189 AC5:" in test
    ac6_main_call = "ac3189_partial_impact_upper_bound_unified();" in test
    ac6_wired = "check_partial_impact_upper_bound_3189" in build
    ac6_wired_after_3188 = (
        build.find("check_partial_impact_upper_bound_3189")
        > build.find("check_production_facade_minimal_ir_shape_3188")
        if ac6_wired
        else False
    )
    no_design = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3189-*")):
            no_design = False
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")
    no_issues_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC6: tests/issues/test_issue_3189.cpp present (forbidden per #81934)")
    ac6_ok = (
        ac6_test_extended
        and ac6_ac1
        and ac6_ac2
        and ac6_ac3
        and ac6_ac4
        and ac6_ac5
        and ac6_main_call
        and ac6_wired
        and ac6_wired_after_3188
        and no_design
        and no_issues_test
    )
    if not ac6_test_extended:
        fails.append("AC6: tests/compiler/test_partial_cone_cap.cpp must extend with ac3189_* source-cite ACs")
    if not ac6_ac1:
        fails.append("AC6: ac3189 AC1 source-cite AC missing in test_partial_cone_cap.cpp")
    if not ac6_ac2:
        fails.append("AC6: ac3189 AC2 source-cite AC missing in test_partial_cone_cap.cpp")
    if not ac6_ac3:
        fails.append("AC6: ac3189 AC3 source-cite AC missing in test_partial_cone_cap.cpp")
    if not ac6_ac4:
        fails.append("AC6: ac3189 AC4 source-cite AC missing in test_partial_cone_cap.cpp")
    if not ac6_ac5:
        fails.append("AC6: ac3189 AC5 source-cite AC missing in test_partial_cone_cap.cpp")
    if not ac6_main_call:
        fails.append("AC6: ac3189_partial_impact_upper_bound_unified() must be called from run_test_partial_cone_cap()")
    if not ac6_wired:
        fails.append("AC6: linter not wired in build.py")
    if not ac6_wired_after_3188:
        fails.append("AC6: linter must be wired in build.py AFTER #3188 linter (issue-number ordering)")
    rows.append(
        {
            "ac": "AC6_no_invent",
            "ok": ac6_ok,
            "test_extended": ac6_test_extended,
            "ac3189_1": ac6_ac1,
            "ac3189_2": ac6_ac2,
            "ac3189_3": ac6_ac3,
            "ac3189_4": ac6_ac4,
            "ac3189_5": ac6_ac5,
            "main_call": ac6_main_call,
            "linter_wired": ac6_wired,
            "linter_wired_after_3188": ac6_wired_after_3188,
            "no_design_docs": no_design,
            "no_issue_test": no_issues_test,
        }
    )

    # ── Report ──
    if args.json:
        out = {"ok": len(fails) == 0, "rows": rows, "fails": fails}
        print(json.dumps(out, indent=2))
        return 0 if (len(fails) == 0 or not args.strict) else 1

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    for r in rows:
        print(f"OK  {r['ac']}")
    print(
        "\nOK: Issue #3189 unify impact upper-bound — every production partial decision site now calls should_partial_relower_impact_checked (#3034 + #2246 + #3189 invalidate_bridge_with_impact)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
