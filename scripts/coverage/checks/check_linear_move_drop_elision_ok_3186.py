#!/usr/bin/env python3
"""Issue #3186: JIT Move/Drop elision consults live commit_readiness.

Closes the JIT half of the half-green residual after densify/steal race.
Production code review 2026-08-20 (residual gap analysis of default-branch
sources) found that the JIT hot path (`aura_jit.cpp` linear_safety_probe)
only checked epoch staleness via `aura_jit_linear_epoch_safety_check` and
never re-consulted full live `commit_readiness(...).would_allow_commit`
in the same critical section as the elision decision. Concurrent fiber
steal / densify could advance `rehydrate_miss_invalidate_gen` / EnvFrame
gen AFTER boundary published success but BEFORE the JIT-specialized frame
re-checked, leaving a transient provenance lag window in which Move/Drop
could still elide on a stale readiness face.

#3186 closes the gap by extending the #3130 predicate (which was already
wired into IR via `linear_move_drop_elision_ok()` and `ir_executor_impl.cpp`
L175) to the JIT hot path:

  - New `extern "C"` runtime bridge `aura_jit_linear_move_drop_elision_ok()`
    in `aura_jit_bridge.{h,cpp}` (thin wrapper around the existing
    predicate — reuses `g_linear_fast_path_elide_blocked_production_total`
    counter, no new metric key).
  - Stub in `aura_jit_bridge_stub.cpp` for non-JIT builds (returns 1 =
    elision OK so non-JIT builds behave like Soft/Off).
  - `aura_jit.cpp` `create_runtime_bridge()` adds FunctionCreate +
    runtime reg entry so ORC can resolve the symbol.
  - `aura_jit.cpp` `linear_safety_probe` emits the new call AND OR-combines
    the result with the existing epoch check (`any_unsafe = is_unsafe OR
    not_elision_ok`) so JIT deopts on EITHER failure.
  - Soft / Off zero-cost: the predicate itself short-circuits the production-
    only bump; only a single relaxed load on `commit_readiness.would_allow_commit`.

Contract (one row per AC):
  AC1  aura_jit_bridge.h declares `aura_jit_linear_move_drop_elision_ok`
       with #3186 cite (Issue #3186: JIT Move/Drop elision ...)
  AC2  aura_jit_bridge.cpp defines the extern "C" wrapper which delegates
       to `typed_audit::linear_move_drop_elision_ok()` and reuses the
       existing `g_linear_fast_path_elide_blocked_production_total`
       counter (no new metric key)
  AC3  aura_jit_bridge_stub.cpp has the weak stub returning 1 (elision
       OK) so non-JIT builds behave like Soft/Off
  AC4  aura_jit.cpp's create_runtime_bridge adds FunctionCreate +
       member variable declaration for `fn_linear_move_drop_elision_ok`
  AC5  aura_jit.cpp's runtime reg block registers the bridge so ORC can
       resolve the symbol emitted by linear_safety_probe
  AC6  aura_jit.cpp's linear_safety_probe emits the call AND OR-combines
       the result with the existing epoch check (`any_unsafe`) — JIT
       deopts on either epoch-stale OR readiness-blocked
  AC7  test_occurrence_goal_persist_rehydrate.cpp extended with
       ac3186_1-5 source-cite ACs; linter wired in build.py after
       #3185; no docs/design/3186-* (#1655); no
       tests/issues/test_issue_3186.cpp (#81934)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_linear_move_drop_elision_ok_3186.py            # report
  python3 scripts/coverage/checks/check_linear_move_drop_elision_ok_3186.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_linear_move_drop_elision_ok_3186.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

BRIDGE_H = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
BRIDGE_CPP = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
BRIDGE_STUB = ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp"
AURA_JIT_CPP = ROOT / "src" / "compiler" / "aura_jit.cpp"
TYPED_AUDIT_H = ROOT / "src" / "compiler" / "typed_mutation_audit.h"
TEST_OCCUR = ROOT / "tests" / "compiler" / "test_occurrence_goal_persist_rehydrate.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3186.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def _count(haystack: str, needle: str) -> int:
    if not needle:
        return 0
    n = 0
    pos = 0
    while True:
        p = haystack.find(needle, pos)
        if p == -1:
            break
        n += 1
        pos = p + 1
    return n


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3186 JIT Move/Drop elision commit_readiness contract")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    h = _read(BRIDGE_H)
    cpp = _read(BRIDGE_CPP)
    stub = _read(BRIDGE_STUB)
    jit = _read(AURA_JIT_CPP)
    _read(TYPED_AUDIT_H)
    test = _read(TEST_OCCUR)
    build = _read(BUILD_PY)

    # ── AC1: bridge.h declares aura_jit_linear_move_drop_elision_ok ──
    ac1_decl = "int aura_jit_linear_move_drop_elision_ok(void);" in h
    ac1_cite = "Issue #3186" in h
    ac1_decl_name = "aura_jit_linear_move_drop_elision_ok" in h
    ac1_ok = ac1_decl and ac1_cite and ac1_decl_name
    if not ac1_decl_name:
        fails.append("AC1: aura_jit_linear_move_drop_elision_ok declaration missing in aura_jit_bridge.h")
    if not ac1_decl:
        fails.append(
            "AC1: aura_jit_linear_move_drop_elision_ok must be declared as 'int aura_jit_linear_move_drop_elision_ok(void);' in aura_jit_bridge.h"
        )
    if not ac1_cite:
        fails.append("AC1: aura_jit_bridge.h must cite Issue #3186 near the declaration")
    rows.append(
        {
            "ac": "AC1_bridge_h_declaration",
            "ok": ac1_ok,
            "name_present": ac1_decl_name,
            "void_signature": ac1_decl,
            "cites_3186": ac1_cite,
        }
    )

    # ── AC2: bridge.cpp defines extern "C" wrapper around predicate ──
    ac2_def = 'extern "C" int aura_jit_linear_move_drop_elision_ok(void)' in cpp
    ac2_body_predicate = "typed_audit::linear_move_drop_elision_ok()" in cpp
    ac2_body_return = "linear_move_drop_elision_ok() ? 1 : 0" in cpp
    ac2_reuse_counter = "g_linear_fast_path_elide_blocked_production_total" in cpp
    ac2_cite = "Issue #3186" in cpp
    ac2_ok = ac2_def and ac2_body_predicate and ac2_body_return and ac2_reuse_counter and ac2_cite
    if not ac2_def:
        fails.append('AC2: aura_jit_bridge.cpp must define extern "C" int aura_jit_linear_move_drop_elision_ok(void)')
    if not ac2_body_predicate:
        fails.append("AC2: bridge must delegate to typed_audit::linear_move_drop_elision_ok() (no second proof model)")
    if not ac2_body_return:
        fails.append("AC2: bridge must return bool→int (1 if elision OK, 0 if blocked)")
    if not ac2_reuse_counter:
        fails.append(
            "AC2: bridge comment must reference existing g_linear_fast_path_elide_blocked_production_total (no new metric key)"
        )
    if not ac2_cite:
        fails.append("AC2: aura_jit_bridge.cpp must cite Issue #3186 near the bridge def")
    rows.append(
        {
            "ac": "AC2_bridge_cpp_definition",
            "ok": ac2_ok,
            "extern_c_def": ac2_def,
            "delegates_to_predicate": ac2_body_predicate,
            "bool_to_int_return": ac2_body_return,
            "reuses_counter": ac2_reuse_counter,
            "cites_3186": ac2_cite,
        }
    )

    # ── AC3: stub in aura_jit_bridge_stub.cpp for non-JIT builds ──
    ac3_stub = "__attribute__((weak)) int aura_jit_linear_move_drop_elision_ok" in stub
    (
        stub.find("aura_jit_linear_move_drop_elision_ok") != -1
        and stub.find("aura_jit_linear_move_drop_elision_ok", stub.find("aura_jit_linear_move_drop_elision_ok") + 1)
        != -1
    )
    # Simpler check: the stub body returns 1 within the brace block right
    # after the function signature. We just look for the pattern
    # `aura_jit_linear_move_drop_elision_ok` followed by `return 1` in the
    # stub file. Since the stub file is small we can do a direct regex.
    ac3_returns_1_simple = "int aura_jit_linear_move_drop_elision_ok(void)" in stub and "return 1;" in stub
    ac3_cite = "Issue #3186" in stub
    ac3_ok = ac3_stub and ac3_returns_1_simple and ac3_cite
    if not ac3_stub:
        fails.append("AC3: aura_jit_bridge_stub.cpp must have a weak stub for aura_jit_linear_move_drop_elision_ok")
    if not ac3_returns_1_simple:
        fails.append("AC3: stub must return 1 (elision OK) so non-JIT builds behave like Soft/Off (zero noise)")
    if not ac3_cite:
        fails.append("AC3: aura_jit_bridge_stub.cpp must cite Issue #3186")
    rows.append(
        {
            "ac": "AC3_bridge_stub",
            "ok": ac3_ok,
            "weak_stub_present": ac3_stub,
            "returns_1": ac3_returns_1_simple,
            "cites_3186": ac3_cite,
        }
    )

    # ── AC4: aura_jit.cpp FunctionCreate + member variable ──
    ac4_member = "llvm::Function* fn_linear_move_drop_elision_ok = nullptr;" in jit
    ac4_create = "fn_linear_move_drop_elision_ok = llvm::Function::Create" in jit
    # The FunctionCreate must declare the extern "C" symbol so ORC can
    # resolve the address at runtime. The source uses a multi-line layout
    # (ExternalLinkage on one line, symbol name on the next), so anchor on
    # the FunctionCreate call and check both substrings within a window.
    create_pos = jit.find("fn_linear_move_drop_elision_ok = llvm::Function::Create")
    if create_pos != -1:
        create_window_end = min(len(jit), create_pos + 500)
        create_window = jit[create_pos:create_window_end]
    else:
        create_window = ""
    ac4_extern_linkage = (
        "ExternalLinkage" in create_window and '"aura_jit_linear_move_drop_elision_ok"' in create_window
    )
    # The return type must be i32 (matches the extern "C" int signature).
    ac4_i32_ty = (
        "FunctionType::get(i32_ty, false)" in create_window
        and '"aura_jit_linear_move_drop_elision_ok"' in create_window
    )
    ac4_cite = "Issue #3186" in jit
    ac4_ok = ac4_member and ac4_create and ac4_extern_linkage and ac4_i32_ty and ac4_cite
    if not ac4_member:
        fails.append(
            "AC4: aura_jit.cpp module class must declare 'llvm::Function* fn_linear_move_drop_elision_ok = nullptr;'"
        )
    if not ac4_create:
        fails.append("AC4: aura_jit.cpp create_runtime_bridge must FunctionCreate fn_linear_move_drop_elision_ok")
    if not ac4_extern_linkage:
        fails.append(
            "AC4: FunctionCreate must use ExternalLinkage with the symbol 'aura_jit_linear_move_drop_elision_ok' (matches bridge.cpp def)"
        )
    if not ac4_i32_ty:
        fails.append("AC4: FunctionCreate must declare i32_ty () signature (matches bridge.cpp int return + no args)")
    if not ac4_cite:
        fails.append("AC4: aura_jit.cpp must cite Issue #3186 near the FunctionCreate")
    rows.append(
        {
            "ac": "AC4_create_runtime_bridge",
            "ok": ac4_ok,
            "member_decl": ac4_member,
            "function_create": ac4_create,
            "extern_linkage_symbol": ac4_extern_linkage,
            "i32_void_signature": ac4_i32_ty,
            "cites_3186": ac4_cite,
        }
    )

    # ── AC5: runtime reg block entry so ORC can resolve the symbol ──
    ac5_reg = 'reg("aura_jit_linear_move_drop_elision_ok"' in jit
    # Cast must be (void*)aura_jit_linear_move_drop_elision_ok (the bridge
    # function pointer, no namespace — it's extern "C").
    ac5_cast = "(void*)aura_jit_linear_move_drop_elision_ok" in jit
    ac5_ok = ac5_reg and ac5_cast
    if not ac5_reg:
        fails.append("AC5: aura_jit.cpp runtime reg block must register aura_jit_linear_move_drop_elision_ok")
    if not ac5_cast:
        fails.append(
            'AC5: reg call must cast to (void*)aura_jit_linear_move_drop_elision_ok (matches extern "C" signature)'
        )
    rows.append(
        {
            "ac": "AC5_runtime_reg",
            "ok": ac5_ok,
            "reg_entry": ac5_reg,
            "void_star_cast": ac5_cast,
        }
    )

    # ── AC6: linear_safety_probe emits call + OR with epoch check ──
    ac6_call = "llvm::FunctionCallee(fn_linear_move_drop_elision_ok)" in jit
    ac6_icmp = "not_elision_ok = irb->CreateICmpNE(elision_ok_i, zero32)" in jit
    ac6_or = "any_unsafe = irb->CreateOr(is_unsafe, not_elision_ok)" in jit
    ac6_condbr = "irb->CreateCondBr(any_unsafe, bb_deopt, bb_ok)" in jit
    ac6_cite = "Issue #3186" in jit
    ac6_ok = ac6_call and ac6_icmp and ac6_or and ac6_condbr and ac6_cite
    if not ac6_call:
        fails.append("AC6: linear_safety_probe must emit CreateCall to fn_linear_move_drop_elision_ok")
    if not ac6_icmp:
        fails.append("AC6: linear_safety_probe must ICmpNE elision_ok_i to zero32 (1=OK, 0=blocked)")
    if not ac6_or:
        fails.append("AC6: linear_safety_probe must OR epoch-check is_unsafe with not_elision_ok into any_unsafe")
    if not ac6_condbr:
        fails.append("AC6: linear_safety_probe must branch on any_unsafe (deopt on either failure)")
    if not ac6_cite:
        fails.append("AC6: linear_safety_probe site must cite Issue #3186")
    rows.append(
        {
            "ac": "AC6_linear_safety_probe",
            "ok": ac6_ok,
            "emit_call": ac6_call,
            "icmp_to_zero32": ac6_icmp,
            "or_with_epoch_check": ac6_or,
            "condbr_any_unsafe": ac6_condbr,
            "cites_3186": ac6_cite,
        }
    )

    # ── AC7: test extension + build.py wire-in + no docs/design/ + no tests/issues/ ──
    ac7_test_extended = "ac3186_jit_linear_move_drop_elision_probe" in test
    ac7_ac1 = "ac3186 AC1:" in test
    ac7_ac2 = "ac3186 AC2:" in test
    ac7_ac3 = "ac3186 AC3:" in test
    ac7_ac4 = "ac3186 AC4:" in test
    ac7_ac5 = "ac3186 AC5:" in test
    ac7_main_call = "ac3186_jit_linear_move_drop_elision_probe();" in test
    ac7_wired = "check_linear_move_drop_elision_ok_3186" in build
    ac7_wired_after_3185 = (
        build.find("check_linear_move_drop_elision_ok_3186") > build.find("check_densify_entry_lcp_consult_3185")
        if ac7_wired
        else False
    )
    no_design = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3186-*")):
            no_design = False
            fails.append(f"AC7: docs/design/{f.name} present (forbidden per #1655)")
    no_issues_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC7: tests/issues/test_issue_3186.cpp present (forbidden per #81934)")
    ac7_ok = (
        ac7_test_extended
        and ac7_ac1
        and ac7_ac2
        and ac7_ac3
        and ac7_ac4
        and ac7_ac5
        and ac7_main_call
        and ac7_wired
        and ac7_wired_after_3185
        and no_design
        and no_issues_test
    )
    if not ac7_test_extended:
        fails.append(
            "AC7: tests/compiler/test_occurrence_goal_persist_rehydrate.cpp must extend with ac3186_* source-cite ACs"
        )
    for n, present in (
        (1, ac7_ac1),
        (2, ac7_ac2),
        (3, ac7_ac3),
        (4, ac7_ac4),
        (5, ac7_ac5),
    ):
        if not present:
            fails.append(f"AC7: ac3186 AC{n} source-cite AC missing in test_occurrence_goal_persist_rehydrate.cpp")
    if not ac7_main_call:
        fails.append("AC7: ac3186_jit_linear_move_drop_elision_probe() must be called from main()")
    if not ac7_wired:
        fails.append("AC7: linter not wired in build.py")
    if not ac7_wired_after_3185:
        fails.append("AC7: linter must be wired in build.py AFTER #3185 linter (issue-number ordering)")
    rows.append(
        {
            "ac": "AC7_no_invent",
            "ok": ac7_ok,
            "test_extended": ac7_test_extended,
            "ac3186_1": ac7_ac1,
            "ac3186_2": ac7_ac2,
            "ac3186_3": ac7_ac3,
            "ac3186_4": ac7_ac4,
            "ac3186_5": ac7_ac5,
            "main_call": ac7_main_call,
            "linter_wired": ac7_wired,
            "linter_wired_after_3185": ac7_wired_after_3185,
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
        "\nOK: Issue #3186 JIT Move/Drop elision commit_readiness — bridge + FunctionCreate + reg + linear_safety_probe OR with epoch check (extends #3130 to JIT)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
