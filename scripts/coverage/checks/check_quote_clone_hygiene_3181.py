#!/usr/bin/env python3
"""Issue #3181: clone walk in_quote boundary — binding/ref split residual
of #3154.

Background
----------
`pre_scan` (#3154) treats `NodeTag::Quote` as a **data boundary** and
stops recursion; bindings under quote are not gensym'd. The **clone
walk** still recursively cloned *all* children first, then applied
`rename_binding` on `Let` / `Lambda` / `Define` positions. Without a
pre-scan map entry, the order bug reappeared *inside quote*:

  1. body `Variable` cloned via `resolve_name` → original name
  2. enclosing `Let` later `rename_binding` → gensym `__x_N`
  3. result: `(quote (let ((__x_0 1)) x))` — binding/ref split

Call-head `"quote"` (distinct from `NodeTag::Quote`) was also unrecognized
in `pre_scan` (only `quasiquote` / `unquote` / `unquote-splicing` were).

Fix: thread `in_quote` through `clone_macro_body_at_depth`. When entering
`NodeTag::Quote` or a Call with Variable head `"quote"`, set
`local_in_quote=true` and propagate to recursive children; under quote
skip `rename_binding` / `resolve_name` / name_map writes (transplant
verbatim). pre_scan unchanged.

Contract (one row per AC):
  AC1  clone_macro_body_at_depth has `bool in_quote` parameter
       (forward decl with default + definition)
  AC2  local_in_quote computed at clone walk entry — detects
       NodeTag::Quote OR Call-head "quote"
  AC3  pre_scan still stops at NodeTag::Quote (#3154 unchanged);
       pre_scan NOT augmented with Call-head "quote" — clone walk's
       in_quote flag covers it instead
  AC4  Under local_in_quote: Variable/Let/LetRec/Define/Set +
       Lambda/MacroDef params use transplant (not resolve_name /
       rename_binding); rest-param fallback repair skipped
  AC5  Recursive children clone passes local_in_quote (threaded)
  AC6  tests/compiler/test_unquote_splicing_hygiene.cpp extended with
       #3181 ACs (binding == ref under Quote / Call-head "quote" /
       qq regression / qq+unquote regression / source-cite / cross-flat)
  AC7  this linter wired in build.py; no docs/design/3181-* (per #1655);
       no tests/issues/test_issue_3181.cpp (per #81934 — extends existing
       src/-aligned suite test_unquote_splicing_hygiene.cpp instead)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_quote_clone_hygiene_3181.py            # report
  python3 scripts/coverage/checks/check_quote_clone_hygiene_3181.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_quote_clone_hygiene_3181.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

MACRO_EXPANSION = ROOT / "src" / "compiler" / "macro_expansion.cpp"
MACRO_IXX = ROOT / "src" / "compiler" / "macro_expansion.ixx"
TEST_FILE = ROOT / "tests" / "compiler" / "test_unquote_splicing_hygiene.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3181.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3181 quote clone hygiene linter")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    me = _read(MACRO_EXPANSION)
    _read(MACRO_IXX)
    test = _read(TEST_FILE)
    build = _read(BUILD_PY)

    # ── AC1: clone_macro_body_at_depth in_quote parameter ──
    # Issue #3181 + #2807 collateral: the function also gained an
    # `in_unquote` parameter (clone walk now mirrors pre_scan for both
    # quote and unquote boundaries). Search for the full signature so
    # the linter doesn't false-positive against the older single-flag
    # signature.
    pos_def = me.find("bool in_quote, bool in_unquote) {")
    pos_decl = me.find("bool in_quote = false, bool in_unquote = false);")
    ac1_ok = pos_def != -1 and pos_decl != -1
    if not ac1_ok:
        fails.append("AC1: clone_macro_body_at_depth missing `bool in_quote` parameter")
    rows.append({"ac": "AC1_in_quote_param", "ok": ac1_ok, "def_site": pos_def, "decl_site": pos_decl})

    # ── AC2: local_in_quote computed at entry, detects NodeTag::Quote
    #         OR Call-head "quote" ──
    pos_local = me.find("bool local_in_quote = in_quote;")
    ac2_local = pos_local != -1
    pos_call_quote = me.find('cname == "quote"')
    ac2_call_quote = pos_call_quote != -1
    ac2_ok = ac2_local and ac2_call_quote
    if not ac2_local:
        fails.append("AC2: local_in_quote flag declaration missing")
    if not ac2_call_quote:
        fails.append('AC2: Call-head "quote" recognition missing')
    rows.append(
        {
            "ac": "AC2_local_in_quote_boundary",
            "ok": ac2_ok,
            "local_decl_site": pos_local,
            "call_quote_site": pos_call_quote,
        }
    )

    # ── AC3: pre_scan still stops at NodeTag::Quote (#3154 unchanged).
    # The #3154 comment block + early-return must remain; the new #3181
    # outer wrapper `if (name_map && !local_in_quote)` gates pre_scan
    # invocation but the inner handler is unchanged. Call-head "quote"
    # is NOT added to pre_scan — clone walk's in_quote flag covers it.
    pos_3154_comment = me.find("Issue #3154: NodeTag::Quote is a data boundary")
    pos_prescan_quote = me.find("if (nv.tag == NodeTag::Quote)\n                return;")
    pos_prescan_wrap = me.find("if (name_map && !local_in_quote)")
    ac3_prescan_unchanged = (
        pos_3154_comment != -1
        and pos_prescan_quote != -1
        and pos_prescan_quote > pos_3154_comment
        and pos_prescan_wrap != -1
    )
    # pre_scan must NOT have a Call-head "quote" handler.
    pos_prescan_lambda = me.find("std::function<void(NodeId, int)> pre_scan")
    prescan_window = ""
    if pos_prescan_lambda != -1:
        prescan_window = me[pos_prescan_lambda : pos_prescan_lambda + 3000]
    prescan_no_call_quote = 'cname == "quote"' not in prescan_window
    ac3_ok = ac3_prescan_unchanged and prescan_no_call_quote
    if not ac3_prescan_unchanged:
        fails.append("AC3: pre_scan #3154 Quote early-return not intact")
    if not prescan_no_call_quote:
        fails.append('AC3: pre_scan must NOT add Call-head "quote" handler (#3181 covers it in clone walk)')
    rows.append(
        {
            "ac": "AC3_pre_scan_unchanged",
            "ok": ac3_ok,
            "prescan_quote_intact": ac3_prescan_unchanged,
            "no_call_quote_in_pre_scan": prescan_no_call_quote,
        }
    )

    # ── AC4: Under local_in_quote, transplant instead of resolve_name /
    #         rename_binding / name_map writes ──
    pos_var_local = me.find(
        "if (local_in_quote) {\n                new_id = target.add_variable(transplant(v.sym_id));"
    )
    # Let/LetRec case uses `case NodeTag::LetRec: {` as unique anchor.
    pos_letrec_anchor = me.find("case NodeTag::LetRec: {")
    letrec_window = me[pos_letrec_anchor : pos_letrec_anchor + 600] if pos_letrec_anchor != -1 else ""
    ac4_let = "local_in_quote" in letrec_window and "transplant(v.sym_id)" in letrec_window
    # Define case uses `case NodeTag::Define: {` as unique anchor.
    pos_def_anchor = me.find("case NodeTag::Define: {")
    def_window = me[pos_def_anchor : pos_def_anchor + 400] if pos_def_anchor != -1 else ""
    ac4_def = "local_in_quote" in def_window and "transplant(v.sym_id)" in def_window
    pos_set_local = me.find("if (!local_in_quote && subst) {")
    pos_params_local = me.find("param_syms.push_back(local_in_quote ? transplant(pid) : rename_binding(pid));")
    pos_lambda_rest_local = me.find(
        "if (dotted && !param_syms.empty() && name_map && s_allow_rest_hygiene &&\n                    !local_in_quote)"
    )
    ac4_var = pos_var_local != -1
    ac4_set = pos_set_local != -1
    ac4_params = pos_params_local != -1
    ac4_lambda_rest = pos_lambda_rest_local != -1
    ac4_ok = ac4_var and ac4_let and ac4_def and ac4_set and ac4_params and ac4_lambda_rest
    if not ac4_var:
        fails.append("AC4: Variable case must branch on local_in_quote")
    if not ac4_let:
        fails.append("AC4: Let/LetRec case must branch on local_in_quote")
    if not ac4_def:
        fails.append("AC4: Define case must branch on local_in_quote (distinct from Let/LetRec)")
    if not ac4_set:
        fails.append("AC4: Set case must guard subst lookup with !local_in_quote")
    if not ac4_params:
        fails.append("AC4: Lambda/MacroDef params must branch on local_in_quote")
    if not ac4_lambda_rest:
        fails.append("AC4: Lambda rest-param fallback must guard with !local_in_quote")
    rows.append(
        {
            "ac": "AC4_local_in_quote_branches",
            "ok": ac4_ok,
            "variable": ac4_var,
            "let": ac4_let,
            "define": ac4_def,
            "set": ac4_set,
            "params": ac4_params,
            "lambda_rest": ac4_lambda_rest,
        }
    )

    # ── AC5: Recursive children clone passes local_in_quote ──
    pos_recur = me.find("clone_macro_body_at_depth(target, target_pool, source, source_pool, cid,")
    recur_window = ""
    if pos_recur != -1:
        recur_window = me[pos_recur : pos_recur + 500]
    ac5_threaded = "local_in_quote" in recur_window
    ac5_ok = ac5_threaded
    if not ac5_ok:
        fails.append("AC5: recursive children clone must pass local_in_quote")
    rows.append({"ac": "AC5_recursive_threaded", "ok": ac5_ok, "threaded": ac5_threaded})

    # ── AC6: test file extended with #3181 ACs ──
    test_has_3181_marker = "ac3181: issue stamp" in test
    test_has_3181_helpers = "build_quote_let_node" in test and "build_quote_call_let" in test
    test_has_binding_ref = "binding_sym == ref_sym" in test
    test_has_qq_regression = "AC3181.3" in test and "AC3181.4" in test
    test_has_source_cite = "AC3181.5" in test
    test_has_cross_flat = "AC3181.6" in test
    ac6_ok = (
        test_has_3181_marker
        and test_has_3181_helpers
        and test_has_binding_ref
        and test_has_qq_regression
        and test_has_source_cite
        and test_has_cross_flat
    )
    if not test_has_3181_marker:
        fails.append("AC6: test file missing `ac3181: issue stamp` marker")
    if not test_has_3181_helpers:
        fails.append("AC6: test file missing build_quote_let_node / build_quote_call_let helpers")
    if not test_has_binding_ref:
        fails.append("AC6: test file missing binding == ref assertion")
    if not test_has_qq_regression:
        fails.append("AC6: test file missing AC3181.3 / AC3181.4 regression coverage")
    if not test_has_source_cite:
        fails.append("AC6: test file missing AC3181.5 source-cite AC")
    if not test_has_cross_flat:
        fails.append("AC6: test file missing AC3181.6 cross-FlatAST AC")
    rows.append(
        {
            "ac": "AC6_test_extension",
            "ok": ac6_ok,
            "marker": test_has_3181_marker,
            "helpers": test_has_3181_helpers,
            "binding_ref": test_has_binding_ref,
            "qq_regression": test_has_qq_regression,
            "source_cite": test_has_source_cite,
            "cross_flat": test_has_cross_flat,
        }
    )

    # ── AC7: linter wired in build.py; no docs/design/3181-*;
    #         no tests/issues/test_issue_3181.cpp ──
    linter_wired = "check_quote_clone_hygiene_3181" in build
    no_design_docs = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3181-*")):
            no_design_docs = False
            fails.append(f"AC7: docs/design/{f.name} present (forbidden per #1655)")
    no_issue_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC7: tests/issues/test_issue_3181.cpp present (forbidden per #81934)")
    ac7_ok = linter_wired and no_design_docs and no_issue_test
    if not linter_wired:
        fails.append("AC7: linter not wired in build.py")
    rows.append(
        {
            "ac": "AC7_no_invent",
            "ok": ac7_ok,
            "linter_wired": linter_wired,
            "no_design_docs": no_design_docs,
            "no_issue_test": no_issue_test,
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
    print("\nOK: Issue #3181 quote clone hygiene — in_quote boundary + binding/ref split fix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
