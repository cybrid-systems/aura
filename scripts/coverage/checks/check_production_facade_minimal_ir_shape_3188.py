#!/usr/bin/env python3
"""Issue #3188: production facade minimal IR/shape step (residual of #3150).

After #3150 closed the joint epoch + AOT dirty + reemit loop, the
production facade (hard_invalidate_via_facade) still skipped
prepare_unified_invalidation_pre_cascade_ + mark_body_only_dirty +
invalidate_shape. notify_dirty_define is listener fan-out only — it
does NOT mark ir_cache_v2_ body-dirty or walk dep_graph_.

#3188 closes the dual-track by, under production + facade success,
still driving a minimal IR body-dirty + shape invalidate for the
mutated define under the same mutate_mtx_ the caller already holds.
Soft / Off byte-identical to today (facade returns false → Soft path
body runs as before, zero extra work). No second JIT model. No new
query keys.

Contract (one row per AC):
  AC1  mark_define_dirty production path: after facade success, calls
       prepare_unified_invalidation_pre_cascade_ + mark_body_only_dirty
       + invalidate_shape for the mutated define
  AC2  invalidate_function production path: same minimal IR/shape step
       after facade success
  AC3  Soft / Off: facade returns false → Soft path body runs unchanged
       (zero-cost contract preserved per #3012 / #3043). The IR/shape
       step is inside the facade-success branch (only fires when facade
       took ownership).
  AC4  existing #3112 / #3129 / #3150 sibling ACs preserved (facade
       still owns joint epoch + AOT dirty + reemit)
  AC5  tests/compiler/test_compiler_hot_update_facade.cpp extended with
       ac3188_* source-cite ACs; linter wired in build.py after #3187;
       no docs/design/3188-* (#1655); no tests/issues/test_issue_3188.cpp
       (#81934)

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_production_facade_minimal_ir_shape_3188.py            # report
  python3 scripts/coverage/checks/check_production_facade_minimal_ir_shape_3188.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_production_facade_minimal_ir_shape_3188.py --json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SERVICE_DIRTY = ROOT / "src" / "compiler" / "service_dirty.cpp"
HOT_UPDATE_REGISTRY = ROOT / "src" / "compiler" / "hot_update_registry.cpp"
TEST_FACADE = ROOT / "tests" / "compiler" / "test_compiler_hot_update_facade.cpp"
BUILD_PY = ROOT / "build.py"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3188.cpp"


def _read(rel: Path) -> str:
    if not rel.is_file():
        return ""
    return rel.read_text(encoding="utf-8", errors="replace")


def _function_window(haystack: str, signature: str, before: int = 0, after: int = 50000) -> str:
    """Return the window of haystack around the first signature occurrence.

    Tries to find the function's closing brace `\n}\n` (column-0 brace)
    first; falls back to a fixed char window if not found within `after`.
    """
    pos = haystack.find(signature)
    if pos == -1:
        return ""
    start = max(0, pos - before)
    # Look for the function-level closing brace (column-0 `\n}\n`).
    # The function body may contain many `\n    }\n` blocks (nested
    # closures), so we need the FIRST `\n}\n` that is NOT preceded by
    # leading whitespace — i.e., the column-0 brace that ends the
    # function body. Walk forward until we find it.
    search_end = min(len(haystack), pos + after)
    body = haystack[start:search_end]
    end_offset = -1
    i = 0
    while i < len(body):
        # Look for newline followed by `}` at column 0 (preceded by
        # only whitespace after the newline), then `\n` (end of line).
        if body[i] == "\n" and i + 1 < len(body) and body[i + 1] == "}" and i + 2 < len(body) and body[i + 2] == "\n":
            end_offset = i + 1  # position of the `}`
            break
        i += 1
    if end_offset != -1:
        return body[: end_offset + 1]
    return body


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #3188 production facade minimal IR/shape step")
    parser.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    fails: list[str] = []
    rows: list[dict] = []

    svc = _read(SERVICE_DIRTY)
    hud = _read(HOT_UPDATE_REGISTRY)
    test = _read(TEST_FACADE)
    build = _read(BUILD_PY)

    # ── AC1: mark_define_dirty production path ──
    # Find the function body region by locating the next `void CompilerService::`
    # signature after the function start. This is more robust than column-0
    # brace detection (which can be confused by nested column-0 patterns).
    md_start = svc.find("void CompilerService::mark_define_dirty")
    # Find the next function start AFTER mark_define_dirty to bound the body.
    md_body_end_search = svc.find("\nvoid CompilerService::", md_start + 1) if md_start != -1 else -1
    md_win = (
        svc[md_start:md_body_end_search]
        if md_start != -1 and md_body_end_search != -1
        else svc[md_start : md_start + 50000]
        if md_start != -1
        else ""
    )
    ac1_cite = "Issue #3188 AC1: residual of #3150" in md_win
    ac1_pre = "prepare_unified_invalidation_pre_cascade_(name)" in md_win
    ac1_body = "mark_body_only_dirty()" in md_win
    ac1_soa = "finish_cascade_soa_dirty_sync_(vit->second)" in md_win
    ac1_shape = "invalidate_shape(name)" in md_win
    ac1_ok = ac1_cite and ac1_pre and ac1_body and ac1_soa and ac1_shape
    if not md_win:
        fails.append("AC1: mark_define_dirty function not found in service_dirty.cpp")
    if not ac1_cite:
        fails.append("AC1: mark_define_dirty must cite 'Issue #3188 AC1: residual of #3150' near the IR/shape step")
    if not ac1_pre:
        fails.append("AC1: mark_define_dirty must call prepare_unified_invalidation_pre_cascade_ after facade success")
    if not ac1_body:
        fails.append("AC1: mark_define_dirty must call mark_body_only_dirty after facade success")
    if not ac1_soa:
        fails.append("AC1: mark_define_dirty must call finish_cascade_soa_dirty_sync_ after mark_body_only_dirty")
    if not ac1_shape:
        fails.append("AC1: mark_define_dirty must call invalidate_shape after facade success")
    rows.append(
        {
            "ac": "AC1_mark_define_dirty_ir_shape",
            "ok": ac1_ok,
            "cite": ac1_cite,
            "pre_cascade": ac1_pre,
            "mark_body_only_dirty": ac1_body,
            "soa_sync": ac1_soa,
            "invalidate_shape": ac1_shape,
        }
    )

    # ── AC2: invalidate_function production path ──
    if_start = svc.find("void CompilerService::invalidate_function")
    if_body_end_search = svc.find("\nvoid CompilerService::", if_start + 1) if if_start != -1 else -1
    if_win = (
        svc[if_start:if_body_end_search]
        if if_start != -1 and if_body_end_search != -1
        else svc[if_start : if_start + 50000]
        if if_start != -1
        else ""
    )
    ac2_cite = "Issue #3188 AC1: residual of #3150" in if_win
    ac2_pre = "prepare_unified_invalidation_pre_cascade_(name)" in if_win
    ac2_body = "mark_body_only_dirty()" in if_win
    ac2_shape = "invalidate_shape(name)" in if_win
    ac2_ok = ac2_cite and ac2_pre and ac2_body and ac2_shape
    if not if_win:
        fails.append("AC2: invalidate_function not found in service_dirty.cpp")
    if not ac2_cite:
        fails.append("AC2: invalidate_function must cite 'Issue #3188 AC1: residual of #3150' near the IR/shape step")
    if not ac2_pre:
        fails.append(
            "AC2: invalidate_function must call prepare_unified_invalidation_pre_cascade_ after facade success"
        )
    if not ac2_body:
        fails.append("AC2: invalidate_function must call mark_body_only_dirty after facade success")
    if not ac2_shape:
        fails.append("AC2: invalidate_function must call invalidate_shape after facade success")
    rows.append(
        {
            "ac": "AC2_invalidate_function_ir_shape",
            "ok": ac2_ok,
            "cite": ac2_cite,
            "pre_cascade": ac2_pre,
            "mark_body_only_dirty": ac2_body,
            "invalidate_shape": ac2_shape,
        }
    )

    # ── AC3: Soft / Off zero-cost — IR/shape step inside facade-success branch ──
    facade_call_pos = md_win.find("hard_invalidate_via_facade(")
    ir_step_pos = md_win.find("Issue #3188 AC1: residual of #3150")
    soft_fallback_pos = md_win.find("gc_coord::Scope gc_coord_scope")
    ac3_facade_present = facade_call_pos != -1
    ac3_step_present = ir_step_pos != -1
    ac3_soft_intact = soft_fallback_pos != -1
    ac3_step_after_facade = ac3_facade_present and ac3_step_present and ir_step_pos > facade_call_pos
    ac3_step_before_soft = ac3_step_present and ac3_soft_intact and ir_step_pos < soft_fallback_pos
    ac3_ok = (
        ac3_facade_present and ac3_step_present and ac3_soft_intact and ac3_step_after_facade and ac3_step_before_soft
    )
    if not ac3_facade_present:
        fails.append("AC3: hard_invalidate_via_facade call must be present in mark_define_dirty")
    if not ac3_step_present:
        fails.append("AC3: IR/shape step must be present in mark_define_dirty")
    if not ac3_soft_intact:
        fails.append("AC3: Soft path body (gc_coord::Scope gc_coord_scope) must remain intact")
    if not ac3_step_after_facade:
        fails.append(
            "AC3: IR/shape step must be AFTER the facade call (inside facade-success branch, not unconditional)"
        )
    if not ac3_step_before_soft:
        fails.append(
            "AC3: IR/shape step must be BEFORE the Soft path body (zero-cost on Soft — fires only when facade returns true)"
        )
    rows.append(
        {
            "ac": "AC3_soft_zero_cost",
            "ok": ac3_ok,
            "facade_call_present": ac3_facade_present,
            "ir_step_present": ac3_step_present,
            "soft_path_intact": ac3_soft_intact,
            "step_after_facade_call": ac3_step_after_facade,
            "step_before_soft_fallback": ac3_step_before_soft,
        }
    )

    # ── AC4: existing #3112 / #3129 / #3150 sibling ACs preserved ──
    ac4_3112_facade = svc.count("hard_invalidate_via_facade(") >= 2  # mark_define_dirty + invalidate_function
    ac4_owner_scoped = (
        "aura_aot_note_cross_eval_hard_owner_scoped" in svc or "aura_aot_note_cross_eval_epoch_force_bump" in svc
    )
    ac4_3129_aot = "aura_aot_bump_func_table_epoch()" in hud
    ac4_3150_bridge = "aura_hot_update_bump_bridge_epoch()" in hud
    ac4_3150_defuse = "aura_hot_update_bump_defuse_version()" in hud
    ac4_3150_dirty = "notify_dirty_define(name)" in hud
    ac4_3150_reemit = "decide_and_reemit(" in hud
    ac4_ok = (
        ac4_3112_facade
        and ac4_owner_scoped
        and ac4_3129_aot
        and ac4_3150_bridge
        and ac4_3150_defuse
        and ac4_3150_dirty
        and ac4_3150_reemit
    )
    if not ac4_3112_facade:
        fails.append("AC4: #3112 facade forwarding must remain in both mark_define_dirty + invalidate_function")
    if not ac4_owner_scoped:
        fails.append("AC4: #2841 / #2951 owner-scoped / force-bump epoch path must remain in service_dirty.cpp")
    if not ac4_3129_aot:
        fails.append("AC4: #3129 facade must still bump AOT func table epoch")
    if not ac4_3150_bridge:
        fails.append("AC4: #3150 facade must still bump bridge epoch")
    if not ac4_3150_defuse:
        fails.append("AC4: #3150 facade must still bump defuse version")
    if not ac4_3150_dirty:
        fails.append("AC4: #3150 facade must still publish to dirty set via notify_dirty_define")
    if not ac4_3150_reemit:
        fails.append("AC4: #3150 facade must still route through decide_and_reemit")
    rows.append(
        {
            "ac": "AC4_sibling_invariants",
            "ok": ac4_ok,
            "facade_forwarded_in_both": ac4_3112_facade,
            "owner_scoped_path": ac4_owner_scoped,
            "3129_aot_epoch": ac4_3129_aot,
            "3150_bridge_epoch": ac4_3150_bridge,
            "3150_defuse_version": ac4_3150_defuse,
            "3150_dirty_set": ac4_3150_dirty,
            "3150_decide_and_reemit": ac4_3150_reemit,
        }
    )

    # ── AC5: test extension + build.py wire-in + no docs/design/ + no tests/issues/ ──
    ac5_test_extended = "ac3188_production_facade_minimal_ir_shape" in test
    ac5_marker = "#3188: production facade minimal IR/shape step" in test
    ac5_ac1 = "ac3188 AC1:" in test
    ac5_ac2 = "ac3188 AC2:" in test
    ac5_ac3 = "ac3188 AC3:" in test
    ac5_ac4 = "ac3188 AC4:" in test
    ac5_ac5 = "ac3188 AC5:" in test
    ac5_main_call = "ac3188_production_facade_minimal_ir_shape();" in test
    ac5_wired = "check_production_facade_minimal_ir_shape_3188" in build
    ac5_wired_after_3187 = (
        build.find("check_production_facade_minimal_ir_shape_3188")
        > build.find("check_dual_dep_graph_strict_or_production_3187")
        if ac5_wired
        else False
    )
    no_design = True
    if DOCS_DESIGN_DIR.is_dir():
        for f in sorted(DOCS_DESIGN_DIR.glob("3188-*")):
            no_design = False
            fails.append(f"AC7: docs/design/{f.name} present (forbidden per #1655)")
    no_issues_test = not ISSUES_TEST_DIR.is_file()
    if ISSUES_TEST_DIR.is_file():
        fails.append("AC7: tests/issues/test_issue_3188.cpp present (forbidden per #81934)")
    ac5_ok = (
        ac5_test_extended
        and ac5_marker
        and ac5_ac1
        and ac5_ac2
        and ac5_ac3
        and ac5_ac4
        and ac5_ac5
        and ac5_main_call
        and ac5_wired
        and ac5_wired_after_3187
        and no_design
        and no_issues_test
    )
    if not ac5_test_extended:
        fails.append(
            "AC5: tests/compiler/test_compiler_hot_update_facade.cpp must extend with ac3188_* source-cite ACs"
        )
    if not ac5_marker:
        fails.append("AC5: test must carry the '#3188: production facade minimal IR/shape step' section marker")
    for n, present in (
        (1, ac5_ac1),
        (2, ac5_ac2),
        (3, ac5_ac3),
        (4, ac5_ac4),
        (5, ac5_ac5),
    ):
        if not present:
            fails.append(f"AC5: ac3188 AC{n} source-cite AC missing in test_compiler_hot_update_facade.cpp")
    if not ac5_main_call:
        fails.append("AC5: ac3188_production_facade_minimal_ir_shape() must be called from run_test_issue_3112()")
    if not ac5_wired:
        fails.append("AC5: linter not wired in build.py")
    if not ac5_wired_after_3187:
        fails.append("AC5: linter must be wired in build.py AFTER #3187 linter (issue-number ordering)")
    rows.append(
        {
            "ac": "AC5_no_invent",
            "ok": ac5_ok,
            "test_extended": ac5_test_extended,
            "marker": ac5_marker,
            "ac3188_1": ac5_ac1,
            "ac3188_2": ac5_ac2,
            "ac3188_3": ac5_ac3,
            "ac3188_4": ac5_ac4,
            "ac3188_5": ac5_ac5,
            "main_call": ac5_main_call,
            "linter_wired": ac5_wired,
            "linter_wired_after_3187": ac5_wired_after_3187,
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
        "\nOK: Issue #3188 production facade minimal IR/shape step — closes dual-track residual of #3150 (notify_dirty_define is listener fan-out only)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
