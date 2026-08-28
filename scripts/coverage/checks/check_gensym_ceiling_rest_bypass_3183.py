#!/usr/bin/env python3
"""Issue #3183: gensym ceiling / depth deny mid-clone + rest path share ceiling.

Residual of #2804 / #2811 / #3157. Mid-walk gensym-ceiling / depth deny
returns NULL_NODE for the current binding while sibling nodes already
add_*'d into the target FlatAST are not rolled back via
expand_ckpt.try_restore(). rename_rest_binding_pre never consulted
effective_max_gensym_map_size() — rest gensyms inflated name_map past
the MacroSelfEvo ceiling (#2804 / #2811).

Contract (one row per AC):
  AC1  rename_binding_pre ceiling deny: expand_ckpt.try_restore()
       before return NULL_NODE (mid-walk rollback, mirror steal /
       pass-limit at L2253 / L2768 / L2789)
  AC2  rename_binding ceiling deny: expand_ckpt.try_restore() before
       return NULL_NODE (clone walk parity with rename_binding_pre)
  AC3  rename_rest_binding_pre shares ceiling: gensym_cap check +
       expand_ckpt.try_restore() + return NULL_NODE; do NOT advance
       g_macro_rest_gensym_serial on deny
  AC4  depth deny stderr: stale "falling back to unhygienic
       substitution" replaced with "deny / NULL_NODE" (#3183-tagged
       diagnostic)
  AC5  steal×expand + pass-limit non-regression: existing
       expand_ckpt.try_restore() call sites in non-ceiling-deny paths
       preserved (total ≥ 6 — 3 pre + 3 new ceiling-deny sites)
  AC6  tests/compiler/test_unquote_splicing_hygiene.cpp extended with
       ac3183_1-6 (extends existing src/-aligned suite per #81934;
       no tests/issues/test_issue_3183.cpp; no docs/design/3183-*
       per #1655)
  AC7  this linter wired in build.py; no docs/design/3183-* (per #1655)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_gensym_ceiling_rest_bypass_3183.py            # report
  python3 scripts/coverage/checks/check_gensym_ceiling_rest_bypass_3183.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_gensym_ceiling_rest_bypass_3183.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

MACRO_EXPANSION = ROOT / "src" / "compiler" / "macro_expansion.cpp"
TEST_FILE = ROOT / "tests" / "compiler" / "test_unquote_splicing_hygiene.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3183.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def _lambda_ceiling_deny_window(me: str, lambda_marker: str) -> tuple[int, int]:
    """Find the ceiling-deny branch (note_hygiene_last_limit_reason(GensymCeiling)
    → return aura::ast::NULL_NODE;) inside the lambda starting at lambda_marker.

    Returns (lim_pos, ret_pos) or (-1, -1) if not found. The returned window
    starts 300 chars BEFORE the note_* call to capture const auto cap
    declarations and the Issue #3183 cite comment that precede the if-block.
    Issue #3341: gensym-ceiling deny now goes through note_* so the
    per-fiber last_limit_reason is stamped (process atomic is last-writer-wins).
    """
    lam_pos = me.find(lambda_marker)
    if lam_pos == -1:
        return (-1, -1)
    lim_pos = me.find("note_hygiene_last_limit_reason(kHygieneLimitReasonGensymCeiling)", lam_pos)
    if lim_pos == -1:
        return (-1, -1)
    ret_pos = me.find("return aura::ast::NULL_NODE;", lim_pos)
    # Extend the window back 300 chars to capture pre-store declarations.
    return (max(lam_pos, lim_pos - 300), ret_pos)


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3183 gensym ceiling / depth deny + rest path linter")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    me = _read(MACRO_EXPANSION)
    test = _read(TEST_FILE)
    build = _read(BUILD_PY)

    if not me:
        fails.append("source: src/compiler/macro_expansion.cpp not readable")

    # ── AC1: rename_binding_pre ceiling deny → expand_ckpt.try_restore() ──
    ac1_cites = "Issue #3183" in me
    lim_pos, ret_pos = _lambda_ceiling_deny_window(me, "rename_binding_pre = [&](SymId sid)")
    ac1_lambda_found = lim_pos != -1 and ret_pos != -1
    ac1_try_restore = False
    if ac1_lambda_found:
        win = me[lim_pos:ret_pos]
        ac1_try_restore = "expand_ckpt.try_restore()" in win
    ac1_ok = ac1_cites and ac1_lambda_found and ac1_try_restore
    if not ac1_cites:
        fails.append("AC1: cite #3183 missing from macro_expansion.cpp")
    if not ac1_lambda_found:
        fails.append("AC1: rename_binding_pre ceiling deny branch not locatable")
    if not ac1_try_restore:
        fails.append("AC1: rename_binding_pre ceiling deny must call expand_ckpt.try_restore() before return")
    rows.append(
        {
            "ac": "AC1_rename_binding_pre_ceiling_rollback",
            "ok": ac1_ok,
            "cites_3183": ac1_cites,
            "lambda_found": ac1_lambda_found,
            "try_restore": ac1_try_restore,
        }
    )

    # ── AC2: rename_binding ceiling deny → expand_ckpt.try_restore() ──
    lim_pos, ret_pos = _lambda_ceiling_deny_window(me, "auto rename_binding = [&](SymId sid)")
    ac2_lambda_found = lim_pos != -1 and ret_pos != -1
    ac2_try_restore = False
    if ac2_lambda_found:
        win = me[lim_pos:ret_pos]
        ac2_try_restore = "expand_ckpt.try_restore()" in win
    ac2_ok = ac2_lambda_found and ac2_try_restore
    if not ac2_lambda_found:
        fails.append("AC2: rename_binding ceiling deny branch not locatable")
    if not ac2_try_restore:
        fails.append("AC2: rename_binding ceiling deny must call expand_ckpt.try_restore() before return")
    rows.append(
        {
            "ac": "AC2_rename_binding_ceiling_rollback",
            "ok": ac2_ok,
            "lambda_found": ac2_lambda_found,
            "try_restore": ac2_try_restore,
        }
    )

    # ── AC3: rename_rest_binding_pre shares ceiling ──
    lim_pos, ret_pos = _lambda_ceiling_deny_window(me, "auto rename_rest_binding_pre = [&](SymId sid)")
    ac3_lambda_found = lim_pos != -1 and ret_pos != -1
    ac3_checks_cap = False
    ac3_checks_size = False
    ac3_try_restore = False
    ac3_no_serial = False
    ac3_cites = False
    if ac3_lambda_found:
        win = me[lim_pos:ret_pos]
        ac3_checks_cap = "effective_max_gensym_map_size()" in win
        ac3_checks_size = "name_map->size() >= " in win
        ac3_try_restore = "expand_ckpt.try_restore()" in win
        ac3_no_serial = "g_macro_rest_gensym_serial.fetch_add" not in win
        # Cite can be in a long comment above the if-block (e.g.,
        # Issue #3183 fix rationale + const auto cap declaration +
        # if condition + store(1,) ≈ 500+ chars). Search the wider
        # lambda body for the cite, not just the deny-window.
        lam_pos = me.find("auto rename_rest_binding_pre = [&](SymId sid)")
        lam_end = me.find("};\n", lam_pos) if lam_pos != -1 else -1
        body = me[lam_pos:lam_end] if (lam_pos != -1 and lam_end != -1) else ""
        ac3_cites = "Issue #3183" in body
    ac3_ok = ac3_lambda_found and ac3_checks_cap and ac3_checks_size and ac3_try_restore and ac3_no_serial and ac3_cites
    if not ac3_lambda_found:
        fails.append("AC3: rename_rest_binding_pre ceiling-deny branch not locatable")
    if not ac3_checks_cap:
        fails.append("AC3: rest path must check effective_max_gensym_map_size()")
    if not ac3_checks_size:
        fails.append("AC3: rest path must check name_map->size() >= cap")
    if not ac3_try_restore:
        fails.append("AC3: rest ceiling deny must call expand_ckpt.try_restore()")
    if not ac3_no_serial:
        fails.append("AC3: rest ceiling deny must NOT advance g_macro_rest_gensym_serial")
    if not ac3_cites:
        fails.append("AC3: rest ceiling deny must cite #3183")
    rows.append(
        {
            "ac": "AC3_rest_path_shares_ceiling",
            "ok": ac3_ok,
            "lambda_found": ac3_lambda_found,
            "checks_cap": ac3_checks_cap,
            "checks_size": ac3_checks_size,
            "try_restore": ac3_try_restore,
            "no_serial": ac3_no_serial,
            "cites_3183": ac3_cites,
        }
    )

    # ── AC4: depth deny stale stderr replaced ──
    ac4_stale_removed = "falling back to unhygienic substitution" not in me
    ac4_deny_present = "deny / NULL_NODE" in me
    ac4_3183_diagnostic = "[#3183 warning] clone_macro_body depth-limit hit" in me
    ac4_ok = ac4_stale_removed and ac4_deny_present and ac4_3183_diagnostic
    if not ac4_stale_removed:
        fails.append('AC4: stale "falling back to unhygienic substitution" must be removed')
    if not ac4_deny_present:
        fails.append('AC4: new "deny / NULL_NODE" diagnostic must be present')
    if not ac4_3183_diagnostic:
        fails.append("AC4: #3183-tagged depth-limit diagnostic must be present")
    rows.append(
        {
            "ac": "AC4_depth_deny_stderr_updated",
            "ok": ac4_ok,
            "stale_removed": ac4_stale_removed,
            "deny_present": ac4_deny_present,
            "diagnostic": ac4_3183_diagnostic,
        }
    )

    # ── AC5: steal×expand + pass-limit non-regression (≥6 try_restore sites) ──
    try_restore_count = 0
    pos = 0
    needle = "expand_ckpt.try_restore()"
    while True:
        nxt = me.find(needle, pos)
        if nxt == -1:
            break
        try_restore_count += 1
        pos = nxt + len(needle)
    ac5_count = try_restore_count
    ac5_ok = ac5_count >= 6
    if not ac5_ok:
        fails.append(f"AC5: expand_ckpt.try_restore() count {ac5_count} < 6 (3 pre + 3 new ceiling-deny)")
    rows.append({"ac": "AC5_steal_pass_limit_non_regression", "ok": ac5_ok, "try_restore_count": ac5_count})

    # ── AC6: test extension ──
    ac6_ac1 = "ac3183_1_ceiling_deny_rolls_back_flatast" in test
    ac6_ac2 = "ac3183_2_rest_path_shares_ceiling" in test
    ac6_ac3 = "ac3183_3_depth_deny_stderr_updated" in test
    ac6_ac4 = "ac3183_4_no_serial_drift_from_rest" in test
    ac6_ac5 = "ac3183_5_steal_pass_limit_non_regression" in test
    ac6_ac6 = "ac3183_6_no_invent" in test
    ac6_called_in_run = "ac3183_1_ceiling_deny_rolls_back_flatast();" in test and "ac3183_6_no_invent();" in test
    ac6_ok = ac6_ac1 and ac6_ac2 and ac6_ac3 and ac6_ac4 and ac6_ac5 and ac6_ac6 and ac6_called_in_run
    if not ac6_ac1:
        fails.append("AC6: test file missing ac3183_1_ceiling_deny_rolls_back_flatast")
    if not ac6_ac2:
        fails.append("AC6: test file missing ac3183_2_rest_path_shares_ceiling")
    if not ac6_ac3:
        fails.append("AC6: test file missing ac3183_3_depth_deny_stderr_updated")
    if not ac6_ac4:
        fails.append("AC6: test file missing ac3183_4_no_serial_drift_from_rest")
    if not ac6_ac5:
        fails.append("AC6: test file missing ac3183_5_steal_pass_limit_non_regression")
    if not ac6_ac6:
        fails.append("AC6: test file missing ac3183_6_no_invent")
    if not ac6_called_in_run:
        fails.append("AC6: test file missing calls to ac3183_* in run_test_unquote_splicing_hygiene")
    rows.append(
        {
            "ac": "AC6_test_extension",
            "ok": ac6_ok,
            "ac1": ac6_ac1,
            "ac2": ac6_ac2,
            "ac3": ac6_ac3,
            "ac4": ac6_ac4,
            "ac5": ac6_ac5,
            "ac6": ac6_ac6,
            "called_in_run": ac6_called_in_run,
        }
    )

    # ── AC7: linter wired, no docs/design/, no tests/issues/ ──
    ac7_wired = "check_gensym_ceiling_rest_bypass_3183" in build
    no_design = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3183-*")):
            no_design = False
            fails.append(f"AC7: docs/design/{f.name} present (forbidden per #1655)")
    no_issues_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC7: tests/issues/test_issue_3183.cpp present (forbidden per #81934)")
    ac7_ok = ac7_wired and no_design and no_issues_test
    if not ac7_wired:
        fails.append("AC7: linter not wired in build.py")
    rows.append(
        {
            "ac": "AC7_no_invent",
            "ok": ac7_ok,
            "linter_wired": ac7_wired,
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
    print("\nOK: Issue #3183 gensym ceiling / depth deny rollback + rest path share ceiling")
    return 0


if __name__ == "__main__":
    sys.exit(main())
