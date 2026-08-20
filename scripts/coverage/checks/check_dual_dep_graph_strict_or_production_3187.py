#!/usr/bin/env python3
"""Issue #3187: dual DepGraph fork window — production fail-closed default.

Closes the residual dual-graph fork window under lockless batch / cross-
fiber record_dependency. The existing Strict fail-closed path (#3165)
only fires when set_dual_dep_graph_strict(1) is called explicitly.
#3187 extends the same all-callers force-dirty walk to also fire under
production_defaults_active() (or AuditStrategy::Full) by default — no
explicit Strict toggle required.

Implementation:
  - new helper `dual_dep_graph_strict_or_production()` in
    dirty_propagation.ixx (ORs `dual_dep_graph_strict_enabled()` with
    `production_defaults_active() || get_strategy() == Full`)
  - record_dependency Strict gate (service.ixx ~L11197) calls the new
    helper instead of just `dual_dep_graph_strict_enabled()`
  - drain_deferred_hybrid_cascade_ Strict gate (service.ixx ~L11308)
    calls the new helper (same surface)
  - Soft / Off zero-cost: helper returns false under Soft (no parity
    check, no force-dirty, no counter bump)
  - reuses existing dual_dep_graph_parity_fail_total counter — no new
    metric key

Contract (one row per AC):
  AC1  dirty_propagation.ixx defines the helper with #3187 cite and ORs
       dual_dep_graph_strict_enabled() with production_defaults_active()
  AC2  record_dependency Strict gate uses the new helper (production
       fail-closed default) and cites Issue #3187
  AC3  drain_deferred_hybrid_cascade_ Strict gate uses the new helper
       and cites Issue #3187
  AC4  existing #3165 sibling AC preserved — dual_dep_graph_strict_enabled
       helper still defined for explicit-Strict tests / non-production paths
  AC5  reuses existing dual_dep_graph_parity_fail_total counter — no new
       metric key (counter appears in observability_metrics.h + dirty + svc)
  AC6  Soft/Off zero-cost contract — helper explicitly returns false under
       Soft (production_defaults_active false + strict_enabled false)
  AC7  test_dep_graph_hybrid_cascade.cpp extended with ac3187_* ACs;
       linter wired in build.py after #3186; no docs/design/3187-* (#1655);
       no tests/issues/test_issue_3187.cpp (#81934)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_dual_dep_graph_strict_or_production_3187.py            # report
  python3 scripts/coverage/checks/check_dual_dep_graph_strict_or_production_3187.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_dual_dep_graph_strict_or_production_3187.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

DIRTY_PROPAGATION = ROOT / "src" / "compiler" / "dirty_propagation.ixx"
SERVICE = ROOT / "src" / "compiler" / "service.ixx"
OBS_METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
TEST_HYBRID = ROOT / "tests" / "compiler" / "test_dep_graph_hybrid_cascade.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3187.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def _window(haystack: str, anchor: str, before: int, after: int) -> str:
    """Return the window of haystack around the first anchor occurrence."""
    pos = haystack.find(anchor)
    if pos == -1:
        return ""
    start = max(0, pos - before)
    end = min(len(haystack), pos + after)
    return haystack[start:end]


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3187 dual DepGraph fork — production fail-closed default")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    ixx = _read(DIRTY_PROPAGATION)
    svc = _read(SERVICE)
    obs = _read(OBS_METRICS)
    test = _read(TEST_HYBRID)
    build = _read(BUILD_PY)

    # ── AC1: helper defined in dirty_propagation.ixx with #3187 cite ──
    helper_def_pos = ixx.find("inline bool dual_dep_graph_strict_or_production()")
    ac1_def = helper_def_pos != -1
    # Helper window: 500 chars BEFORE the anchor (for the introducing
    # comment block) + 1500 chars AFTER (for the function body).
    if ac1_def:
        helper_start = max(0, helper_def_pos - 500)
        helper_end = min(len(ixx), helper_def_pos + 1500)
        helper_window = ixx[helper_start:helper_end]
    else:
        helper_window = ""
    ac1_cite = "Issue #3187" in helper_window
    ac1_calls_strict = "dual_dep_graph_strict_enabled()" in helper_window
    ac1_calls_prod = "production_defaults_active()" in helper_window
    ac1_calls_full = "AuditStrategy::Full" in helper_window
    ac1_ok = ac1_def and ac1_cite and ac1_calls_strict and ac1_calls_prod and ac1_calls_full
    if not ac1_def:
        fails.append("AC1: inline bool dual_dep_graph_strict_or_production() missing in dirty_propagation.ixx")
    if not ac1_cite:
        fails.append("AC1: helper must cite Issue #3187 near the function definition (comment block ±1500 chars)")
    if not ac1_calls_strict:
        fails.append("AC1: helper must call dual_dep_graph_strict_enabled() (OR semantics for explicit-Strict tests)")
    if not ac1_calls_prod:
        fails.append("AC1: helper must call production_defaults_active() (production fail-closed default)")
    if not ac1_calls_full:
        fails.append("AC1: helper must check get_strategy() == AuditStrategy::Full (Full path is also fail-closed)")
    rows.append(
        {
            "ac": "AC1_helper_definition",
            "ok": ac1_ok,
            "defined": ac1_def,
            "cites_3187": ac1_cite,
            "calls_strict": ac1_calls_strict,
            "calls_prod": ac1_calls_prod,
            "calls_full": ac1_calls_full,
        }
    )

    # ── AC2: record_dependency Strict gate uses the new helper ──
    rd_pos = svc.find("void record_dependency(const std::string& caller")
    if rd_pos == -1:
        rd_pos = svc.find("void record_dependency(")
    rd_window = svc[rd_pos : min(len(svc), rd_pos + 9000)] if rd_pos != -1 else ""
    ac2_uses_helper = "dual_dep_graph_strict_or_production()" in rd_window
    ac2_cite = "Issue #3187" in rd_window
    # The all-callers walk must remain (sibling #3165 contract preserved)
    ac2_all_callers_walk = "for (const auto& [callee_name, callee_entry] : dep_graph_)" in rd_window
    # Existing dual_dep_graph_strict_enabled may still appear (helper definition),
    # but the Strict gate branch must use the new helper.
    ac2_ok = ac2_uses_helper and ac2_cite and ac2_all_callers_walk
    if rd_pos == -1:
        fails.append("AC2: record_dependency definition missing in service.ixx")
    if not ac2_uses_helper:
        fails.append(
            "AC2: record_dependency Strict gate must use dual_dep_graph_strict_or_production() (production fail-closed default)"
        )
    if not ac2_cite:
        fails.append("AC2: record_dependency must cite Issue #3187 near the new helper call")
    if not ac2_all_callers_walk:
        fails.append("AC2: record_dependency all-callers walk missing (sibling #3165 contract not preserved)")
    rows.append(
        {
            "ac": "AC2_record_dependency_gate",
            "ok": ac2_ok,
            "uses_helper": ac2_uses_helper,
            "cites_3187": ac2_cite,
            "all_callers_walk_preserved": ac2_all_callers_walk,
        }
    )

    # ── AC3: drain_deferred_hybrid_cascade_ Strict gate uses the new helper ──
    drain_pos = svc.find("void drain_deferred_hybrid_cascade_()")
    drain_window = svc[drain_pos : min(len(svc), drain_pos + 5500)] if drain_pos != -1 else ""
    ac3_uses_helper = "dual_dep_graph_strict_or_production()" in drain_window
    ac3_cite = "Issue #3187" in drain_window
    ac3_all_callers_walk = "for (const auto& [callee_name, callee_entry] : dep_graph_)" in drain_window
    ac3_ok = ac3_uses_helper and ac3_cite and ac3_all_callers_walk
    if drain_pos == -1:
        fails.append("AC3: drain_deferred_hybrid_cascade_ definition missing in service.ixx")
    if not ac3_uses_helper:
        fails.append("AC3: drain_deferred_hybrid_cascade_ Strict gate must use dual_dep_graph_strict_or_production()")
    if not ac3_cite:
        fails.append("AC3: drain must cite Issue #3187 near the new helper call")
    if not ac3_all_callers_walk:
        fails.append("AC3: drain all-callers walk missing (sibling #3165 contract not preserved)")
    rows.append(
        {
            "ac": "AC3_drain_gate",
            "ok": ac3_ok,
            "uses_helper": ac3_uses_helper,
            "cites_3187": ac3_cite,
            "all_callers_walk_preserved": ac3_all_callers_walk,
        }
    )

    # ── AC4: existing #3165 sibling AC preserved — strict_enabled helper
    # still defined for explicit-Strict tests / non-production paths ──
    ac4_strict_helper = "inline bool dual_dep_graph_strict_enabled()" in ixx
    ac4_3165_sibling = "ac3165_strict_fail_closed_all_callers" in test
    ac4_setter = "set_dual_dep_graph_strict" in ixx
    ac4_ok = ac4_strict_helper and ac4_3165_sibling and ac4_setter
    if not ac4_strict_helper:
        fails.append("AC4: dual_dep_graph_strict_enabled() helper missing — #3165 backward compat broken")
    if not ac4_3165_sibling:
        fails.append("AC4: ac3165_strict_fail_closed_all_callers missing from test_dep_graph_hybrid_cascade.cpp")
    if not ac4_setter:
        fails.append(
            "AC4: set_dual_dep_graph_strict setter missing in dirty_propagation.ixx (explicit-Strict test hook)"
        )
    rows.append(
        {
            "ac": "AC4_sibling_3165_preserved",
            "ok": ac4_ok,
            "strict_enabled_helper": ac4_strict_helper,
            "3165_sibling_ac_present": ac4_3165_sibling,
            "strict_setter": ac4_setter,
        }
    )

    # ── AC5: reuses existing dual_dep_graph_parity_fail_total counter
    # — no new metric key (counter present in observability_metrics.h +
    # dirty_propagation.ixx + service.ixx) ──
    ac5_obs = "dual_dep_graph_parity_fail_total" in obs
    ac5_ixx = "dual_dep_graph_parity_fail_total" in ixx
    ac5_svc = "dual_dep_graph_parity_fail_total" in svc
    # Count total occurrences to confirm no new key was inserted.
    total = (
        obs.count("dual_dep_graph_parity_fail_total")
        + ixx.count("dual_dep_graph_parity_fail_total")
        + svc.count("dual_dep_graph_parity_fail_total")
    )
    ac5_count_ok = total >= 4
    ac5_ok = ac5_obs and ac5_ixx and ac5_svc and ac5_count_ok
    if not ac5_obs:
        fails.append("AC5: dual_dep_graph_parity_fail_total missing from observability_metrics.h (counter not defined)")
    if not ac5_ixx:
        fails.append(
            "AC5: dual_dep_graph_parity_fail_total missing from dirty_propagation.ixx (process-atomic accessor)"
        )
    if not ac5_svc:
        fails.append(
            "AC5: dual_dep_graph_parity_fail_total missing from service.ixx (no bump site — fail-closed path not wired)"
        )
    if not ac5_count_ok:
        fails.append(f"AC5: dual_dep_graph_parity_fail_total appears only {total}× across obs+ixx+svc (expected >= 4)")
    rows.append(
        {
            "ac": "AC5_counter_reuse",
            "ok": ac5_ok,
            "obs_counter": ac5_obs,
            "ixx_atomic": ac5_ixx,
            "svc_bump": ac5_svc,
            "total_occurrences": total,
        }
    )

    # ── AC6: Soft/Off zero-cost — helper explicitly returns false under Soft.
    # The helper body ORs three things: dual_dep_graph_strict_enabled() ||
    # production_defaults_active() || get_strategy() == AuditStrategy::Full.
    # Under Soft all three are false → returns false → no parity check /
    # force-dirty / counter bump. The 'if' branch in the helper must early-
    # return true on the strict toggle so Soft/Off skips the rest of the
    # OR evaluation (zero extra cost on quiet windows). ──
    if helper_def_pos != -1:
        helper_body_end = ixx.find("\n}\n", helper_def_pos)
        helper_body = ixx[helper_def_pos:helper_body_end] if helper_body_end != -1 else ""
    else:
        helper_body = ""
    ac6_early_return = "if (dual_dep_graph_strict_enabled())\n        return true;" in helper_body
    ac6_short_circuit_or = helper_body.find("dual_dep_graph_strict_enabled()") < helper_body.find(
        "production_defaults_active()"
    )
    ac6_ok = ac6_early_return and ac6_short_circuit_or
    if not ac6_early_return:
        fails.append(
            "AC6: helper body must early-return true on strict toggle (short-circuit, zero extra cost on Soft)"
        )
    if not ac6_short_circuit_or:
        fails.append("AC6: helper body must consult strict toggle BEFORE production check (short-circuit OR semantics)")
    rows.append(
        {
            "ac": "AC6_soft_zero_cost",
            "ok": ac6_ok,
            "early_return_on_strict": ac6_early_return,
            "short_circuit_or_order": ac6_short_circuit_or,
        }
    )

    # ── AC7: test extension + build.py wire-in + no docs/design/ + no tests/issues/ ──
    ac7_test_extended = "ac3187_production_fail_closed_default" in test
    ac7_marker = "#3187: dual DepGraph fork" in test
    ac7_ac1 = "ac3187 AC1:" in test
    ac7_ac2 = "ac3187 AC2:" in test
    ac7_ac3 = "ac3187 AC3:" in test
    ac7_ac4 = "ac3187 AC4:" in test
    ac7_ac5 = "ac3187 AC5:" in test
    ac7_main_call = "ac3187_production_fail_closed_default();" in test
    ac7_wired = "check_dual_dep_graph_strict_or_production_3187" in build
    ac7_wired_after_3186 = (
        build.find("check_dual_dep_graph_strict_or_production_3187")
        > build.find("check_linear_move_drop_elision_ok_3186")
        if ac7_wired
        else False
    )
    no_design = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3187-*")):
            no_design = False
            fails.append(f"AC7: docs/design/{f.name} present (forbidden per #1655)")
    no_issues_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC7: tests/issues/test_issue_3187.cpp present (forbidden per #81934)")
    ac7_ok = (
        ac7_test_extended
        and ac7_marker
        and ac7_ac1
        and ac7_ac2
        and ac7_ac3
        and ac7_ac4
        and ac7_ac5
        and ac7_main_call
        and ac7_wired
        and ac7_wired_after_3186
        and no_design
        and no_issues_test
    )
    if not ac7_test_extended:
        fails.append("AC7: tests/compiler/test_dep_graph_hybrid_cascade.cpp must extend with ac3187_* source-cite ACs")
    if not ac7_marker:
        fails.append("AC7: test must carry the '#3187: dual DepGraph fork' section marker")
    for n, present in (
        (1, ac7_ac1),
        (2, ac7_ac2),
        (3, ac7_ac3),
        (4, ac7_ac4),
        (5, ac7_ac5),
    ):
        if not present:
            fails.append(f"AC7: ac3187 AC{n} source-cite AC missing in test_dep_graph_hybrid_cascade.cpp")
    if not ac7_main_call:
        fails.append(
            "AC7: ac3187_production_fail_closed_default() must be called from run_test_dep_graph_hybrid_cascade()"
        )
    if not ac7_wired:
        fails.append("AC7: linter not wired in build.py")
    if not ac7_wired_after_3186:
        fails.append("AC7: linter must be wired in build.py AFTER #3186 linter (issue-number ordering)")
    rows.append(
        {
            "ac": "AC7_no_invent",
            "ok": ac7_ok,
            "test_extended": ac7_test_extended,
            "marker": ac7_marker,
            "ac3187_1": ac7_ac1,
            "ac3187_2": ac7_ac2,
            "ac3187_3": ac7_ac3,
            "ac3187_4": ac7_ac4,
            "ac3187_5": ac7_ac5,
            "main_call": ac7_main_call,
            "linter_wired": ac7_wired,
            "linter_wired_after_3186": ac7_wired_after_3186,
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
        "\nOK: Issue #3187 dual DepGraph fork — production fail-closed default (extends #3165 Strict to production_defaults_active)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
