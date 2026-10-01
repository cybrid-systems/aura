#!/usr/bin/env python3
# scripts/check_safe_refactor_typecheck_4266.py -- Issue #4266 gate.
#
# AC1: structured status primitive — (typecheck-status) registered in
#      evaluator_primitives_eval.cpp citing #4266, returning #t on clean /
#      the diagnostics report string on type errors / #f without workspace;
#      it and (typecheck-current) both call the shared run_workspace_typecheck
#      core (one traversal, one Phase-5 cache, one coverage counter — no
#      second model, no duplicated infer_flat); the cache parity field
#      last_typecheck_ok_ exists beside last_typecheck_result_ in
#      evaluator.ixx.
# AC2: stdlib gate — safe-refactor:replace-fn calls (typecheck-status)
#      AFTER (set-code rebuilt) and reports 'applied ONLY on the structured
#      ok status (#t); the not-ok branch restores the snapshot and rides
#      the diagnostics report in the rejected reason slot; NO live
#      (non-comment) stdlib code gates on the raw (typecheck-current)
#      report. check-and-apply runs post-verify INSIDE the restore path:
#      a throw yields (list 'error "post-verify-error-raised") WITH
#      ast:restore, never an escape leaving the snapshot mutated.
# AC3: test family — ac4266_1..ac4266_6 are declared AND dispatched in
#      run_test_current_source_roundtrip (tests/compiler/
#      test_current_source_roundtrip.cpp); tests/python/run-tests.sh cites
#      #4266 with the tc-status-clean / replace-applies / replace-rejects /
#      post-verify-throw smoke cases (real CLI + real module require face).
# AC4: wiring — build.py registers this linter, the filename is on the
#      frozen root_check_allowlist, and no docs/design/4266-* or
#      tests/**/test_issue_4266* exists (per #1655 / #81934).
#
# Exit 0 = all rows satisfied.

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PRIM = "src/compiler/evaluator_primitives_eval.cpp"
IXX = "src/compiler/evaluator.ixx"
UNPARSE = "src/core/ast_unparse.ixx"
STDLIB = "lib/std/safe-refactor.aura"
TEST = "tests/compiler/test_current_source_roundtrip.cpp"
RUNTESTS = "tests/python/run-tests.sh"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

AC_FUNCS = [
    "ac4266_1_structured_status",
    "ac4266_2_replace_fn_applies_well_typed",
    "ac4266_3_replace_fn_rejects_and_restores",
    "ac4266_4_check_apply_post_verify_throw_restores",
    "ac4266_5_check_apply_post_verify_false_still_rolls_back",
    "ac4266_6_source_cite",
    "ac4266_7_set_code_invalidates_typecheck_cache",
]

SMOKE_CASES = [
    "safe-refactor:tc-status-clean",
    "safe-refactor:replace-applies",
    "safe-refactor:replace-rejects",
    "safe-refactor:post-verify-throw",
]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _strip_aura_comments(src: str) -> str:
    """Drop `;`-to-EOL comment lines so comment mentions of the old arm do
    not satisfy (or trip) live-code checks."""
    return "\n".join(line for line in src.split("\n") if not line.lstrip().startswith(";"))


def _slice(src: str, start: str, end: str) -> str:
    """Slice [start, end) — end of file when the end anchor is empty."""
    pos = src.find(start)
    if pos == -1:
        return ""
    if not end:
        return src[pos:]
    stop = src.find(end, pos + len(start))
    return src[pos:] if stop == -1 else src[pos:stop]


def main() -> int:
    fails: list[str] = []

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    prim = _read(PRIM)
    ixx = _read(IXX)
    unparse = _read(UNPARSE)
    stdlib_live = _strip_aura_comments(_read(STDLIB))
    test = _read(TEST)
    runtests = _read(RUNTESTS)
    build = _read(BUILD)
    allowlist = _read(ALLOWLIST)

    # ── AC1: structured status primitive + shared core ──
    status_body = _slice(prim, 'add("typecheck-status"', '\n    add("')
    must(bool(status_body), "AC1: typecheck-status primitive registered")
    must(prim.find("4266") != -1, "AC1: primitive cites #4266")
    must("run_workspace_typecheck" in status_body, "AC1: typecheck-status routes through the shared core")
    must(
        "make_bool(true)" in status_body and "make_bool(false)" in status_body,
        "AC1: structured #t (clean) / #f (no workspace) faces",
    )
    must("st->report" in status_body, "AC1: diagnostics ride the report string, never the boolean")
    current_body = _slice(prim, 'add("typecheck-current"', 'add("typecheck-status"')
    must("run_workspace_typecheck(ev)" in current_body, "AC1: typecheck-current delegates to the shared core")
    must("infer_flat" not in current_body, "AC1: no duplicated traversal — one typecheck model")
    must(prim.count("run_workspace_typecheck") >= 3, "AC1: shared core defined and called from BOTH primitives")
    must("last_typecheck_ok_" in ixx, "AC1: cache parity field in evaluator.ixx")
    must(
        ixx.find("last_typecheck_ok_") > ixx.find("last_typecheck_result_") != -1
        and ixx.find("last_typecheck_result_") != -1,
        "AC1: parity field cached beside last_typecheck_result_",
    )
    # Issue #4266 follow-up: the Phase-5 report cache must be droppable on a
    # whole-workspace replacement (fresh FlatAST has no dirty bits, so the
    # has_dirty_subtree() gate cannot see set-code/load/restore).
    must(
        "invalidate_typecheck_report_cache" in ixx,
        "AC1: typecheck report cache invalidation hook defined (SSOT, evaluator.ixx)",
    )
    must(
        "invalidate_typecheck_report_cache" in prim,
        "AC1: set-code / load-file paths drop the cached report",
    )
    must(
        "invalidate_typecheck_report_cache" in _read("src/compiler/evaluator_primitives_ast.cpp"),
        "AC1: ast:restore drops the cached report",
    )
    must(
        "invalidate_typecheck_report_cache" in _read("src/compiler/evaluator_eval_flat.cpp"),
        "AC1: snapshot restore drops the cached report",
    )
    # The workspace unparse must keep per-parameter type annotations, or the
    # rebuilt source loses the ground signature the typechecker gates on.
    must(
        "param_annotations" in unparse,
        "AC1: unparse renders Lambda::param_annotations (lossless round-trip)",
    )

    # ── AC2: stdlib gate ──
    replace_body = _slice(stdlib_live, "(define (safe-refactor:replace-fn", "")
    must("(typecheck-status)" in replace_body, "AC2: replace-fn gates on the structured status")
    must(
        replace_body.find("(set-code rebuilt)") != -1
        and replace_body.find("(set-code rebuilt)") < replace_body.find("(typecheck-status)"),
        "AC2: status is taken AFTER set-code (the rebuilt workspace)",
    )
    must("((equal? apply-result #t)" in replace_body, "AC2: applied ONLY on the structured ok status (#t)")
    else_arm = _slice(replace_body, "(else", "")
    must(
        "(ast:restore snap-id)" in else_arm and "(list 'rejected" in else_arm,
        "AC2: not-ok branch restores the snapshot and reports rejected",
    )
    must("(typecheck-current)" not in stdlib_live, "AC2: no live stdlib code gates on the raw typecheck-current report")
    apply_body = _slice(stdlib_live, "(define (safe-refactor:check-and-apply", "(define (safe-refactor:replace-fn")
    must(
        "(try (post-verify) (catch (e) 'post-verify-error-raised))" in apply_body,
        "AC2: post-verify runs inside a try (throw is caught)",
    )
    must('(list \'error "post-verify-error-raised")' in apply_body, "AC2: throw face reported as structured error")
    pv_arm = _slice(apply_body, "(try (post-verify)", "(not pv)")
    must("(ast:restore snap-id)" in pv_arm, "AC2: throw branch restores the snapshot (was: escaped, no restore)")
    must(
        '(list \'rolled-back "post-verify-failed")' in apply_body,
        "AC2: #f face still rolls back (prior behavior pinned)",
    )

    # ── AC3: test family declared AND dispatched ──
    for name in AC_FUNCS:
        must(f"static void {name}(" in test, f"AC3: {name} declared")
        must(f"{name}();" in test, f"AC3: {name} dispatched in the runner")
    must("Issue #4266" in test, "AC3: test section cites #4266")
    must("4266" in runtests, "AC3: run-tests.sh cites #4266")
    for case in SMOKE_CASES:
        must(f'run_test "{case}"' in runtests, f"AC3: run-tests.sh case {case}")

    # ── AC4: wiring ──
    must(
        build.count("check_safe_refactor_typecheck_4266") >= 2, "AC4: build.py registers this linter (path + fail hint)"
    )
    must("check_safe_refactor_typecheck_4266.py" in allowlist, "AC4: filename on the frozen root_check_allowlist")
    must(not list(ROOT.glob("docs/design/4266-*")), "AC4: no docs/design/4266-* per #1655")
    must(not list(ROOT.glob("tests/**/test_issue_4266*")), "AC4: no tests/**/test_issue_4266* per #81934")

    for ac, label in (
        ("AC1", "structured status primitive"),
        ("AC2", "stdlib replace-fn / check-and-apply gate"),
        ("AC3", "test family + smoke cases"),
        ("AC4", "wiring + allowlist + hygiene"),
    ):
        rows = [f for f in fails if f.startswith(ac + ":")]
        print(f"  {ac} {label}: {'OK' if not rows else 'FAIL'}")

    if fails:
        print(f"FAIL scripts/check_safe_refactor_typecheck_4266.py ({len(fails)} row(s)):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK scripts/check_safe_refactor_typecheck_4266.py — all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
