#!/usr/bin/env python3
"""Issue #4155 source-cite gate: Phase-5 Moving densify success must
clear the residual CastOp persist NodeIds on the same #3985
production/Full invalidation surface (false-attribute under partial).

#3985's force_ir_cache_map_invalid_after_densify clears source_to_ir_map,
sets abort_map_invalid, drops content_stored_this_epoch and marks dirty —
but leaves t_residual_castop_ast / t_residual_castop_blocks holding
pre-densify NodeIds. After a later store_define_v2 rebuilds the map, a
recycled NodeId lets mark_entry_from_dead_coercion_persist_ attribute the
WRONG block while the real type-changed CastOp site stays clean; #3618
fail-closes only when attribution FAILS, so a false-positive bypasses the
fail-closed gate. Densify now drops the stale persist and bumps the
#3102 decision-invalidate gen (DeadCoercion full-scan). Soft / Off keep
zero cost (the densify hook returns before the persist drop).

ACs:
  AC1  service.ixx force_ir_cache_map_invalid_after_densify: on the
       production/Full face (after the Soft/Off early return) the helper
       calls aura::compiler::dirty::reset_residual_castop_persist_after_densify
       and the call site cites #4155.
  AC2  dirty_propagation.ixx reset_residual_castop_persist_after_densify:
       clears t_residual_castop_ast AND t_residual_castop_blocks and bumps
       bump_dead_coercion_decision_invalidate; empty persist early-returns
       (no bump); the comment cites #4155 + the #3618 false-positive
       rationale (remap unavailable — no densify NodeId map reaches this
       surface, so drop instead).
  AC3  No new model / no invent: the clear reuses the existing persist
       vectors and the existing #3102 decision-invalidate atomic (no new
       TLS vector / counter / query key / schema-4155 row); the #4154
       remirror latch (note_pending_full_solve_residual /
       residual_persist_sites_durable) and the #3618 fail-closed row
       (!persist_attributed) are unchanged; the abort path
       (force_ir_cache_dirty_after_abort) is untouched.
  AC4  Runtime door + wiring: ac4155_1..ac4155_4 defined in and registered
       by run_test_dead_coercion_dirty_cone in
       tests/compiler/test_dead_coercion_dirty_cone.cpp; this linter is
       registered in build.py and listed in
       scripts/coverage/root_check_allowlist.txt; no
       tests/**/test_issue_4155.cpp (per #81934); no docs/design/4155-*
       (per #1655).
  AC5  Soft / Off zero-cost: the persist drop sits after the
       production/Full early return in force_ir_cache_map_invalid_after_densify
       (Soft never reaches it); reset_residual_castop_persist_for_test
       still clears all three persist TLS bits (test isolation unchanged).

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRTY = ROOT / "src" / "compiler" / "dirty_propagation.ixx"
SVC = ROOT / "src" / "compiler" / "service.ixx"
TST = ROOT / "tests" / "compiler" / "test_dead_coercion_dirty_cone.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, row: str) -> None:
    if not cond:
        failures.append(row)


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i:j] if j > i else text[i:]


def main() -> int:
    dirty = DIRTY.read_text()
    svc = SVC.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: the #3985 densify surface drops the stale persist ──────
    svc_fn = svc.find("void force_ir_cache_map_invalid_after_densify()")
    must(svc_fn >= 0, "AC1: #3985 surface present in service.ixx")
    if svc_fn >= 0:
        win = svc[svc_fn : svc_fn + 2200]
        must("reset_residual_castop_persist_after_densify" in win, "AC1: surface drops persist")
        must("Issue #4155" in win, "AC1: surface call site cites #4155")

    # ── AC2: the clear helper contract in dirty_propagation.ixx ─────
    helper = dirty.find("inline void reset_residual_castop_persist_after_densify() noexcept {")
    must(helper >= 0, "AC2: reset_residual_castop_persist_after_densify defined")
    if helper >= 0:
        win = dirty[helper : helper + 900]
        must("t_residual_castop_ast.clear()" in win, "AC2: clears stale AST Ids")
        must("t_residual_castop_blocks.clear()" in win, "AC2: clears stale block Ids")
        must("bump_dead_coercion_decision_invalidate()" in win, "AC2: bumps #3102 gen (no new model)")
        must(
            "t_residual_castop_ast.empty() && t_residual_castop_blocks.empty()" in win,
            "AC2: empty persist early-returns (no bump)",
        )
    must("kResidualCastopDensifyClearIssue = 4155" in dirty, "AC2: issue stamp constant")
    must(
        "false-attribute" in dirty[helper - 1600 : helper] if helper >= 0 else False,
        "AC2: comment carries the #3618 false-positive rationale",
    )

    # ── AC3: no new model; prior faces intact ───────────────────────
    must(
        dirty.count("inline thread_local std::vector<NodeId> t_residual_castop_ast{") == 1,
        "AC3: no second persist TLS vector (reuses #3120 storage)",
    )
    must("note_pending_full_solve_residual" in dirty, "AC3: #4154 latch unchanged")
    must("residual_persist_sites_durable" in dirty, "AC3: #4154 durability probe unchanged")
    must("!persist_attributed" in svc, "AC3: #3618 fail-closed row unchanged")
    must("force_ir_cache_dirty_after_abort" in svc, "AC3: abort densify path untouched")
    must("schema-4155" not in svc and "schema-4155" not in dirty, "AC3: no new query key / schema row")

    # ── AC4: runtime door + wiring ──────────────────────────────────
    must("static void ac4155_1_densify_clears_stale_persist()" in tst, "AC4: ac4155_1 defined")
    must("static void ac4155_2_stale_ids_cannot_drive_remirror()" in tst, "AC4: ac4155_2 defined")
    must("static void ac4155_3_empty_persist_and_soft_zero_cost()" in tst, "AC4: ac4155_3 defined")
    must("static void ac4155_4_source_cite_and_wiring()" in tst, "AC4: ac4155_4 defined")
    runner = _between(tst, "int run_test_dead_coercion_dirty_cone()", "#ifndef AURA_ISSUE_BATCH_MEMBER")
    must("ac4155_1_densify_clears_stale_persist();" in runner, "AC4: ac4155_1 registered in runner")
    must("ac4155_2_stale_ids_cannot_drive_remirror();" in runner, "AC4: ac4155_2 registered in runner")
    must("ac4155_3_empty_persist_and_soft_zero_cost();" in runner, "AC4: ac4155_3 registered in runner")
    must("ac4155_4_source_cite_and_wiring();" in runner, "AC4: ac4155_4 registered in runner")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4155.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4155.cpp (per #81934)",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4155-*")), "AC4: no docs/design/4155-* (per #1655)")
    must("check_residual_castop_densify_clear_4155" in build, "AC4: build.py registers the linter")
    must("check_residual_castop_densify_clear_4155.py" in allow, "AC4: root_check_allowlist.txt lists the linter")

    # ── AC5: Soft / Off zero-cost ordering ──────────────────────────
    if svc_fn >= 0:
        win = svc[svc_fn : svc_fn + 2200]
        gate = win.find("production_defaults_active()")
        drop = win.find("reset_residual_castop_persist_after_densify")
        must(
            gate >= 0 and drop > gate, "AC5: persist drop sits behind the production/Full gate (Soft never reaches it)"
        )
    reset_for_test = dirty.find("inline void reset_residual_castop_persist_for_test() noexcept {")
    must(reset_for_test >= 0, "AC5: test reset retained")
    if reset_for_test >= 0:
        win = dirty[reset_for_test : reset_for_test + 300]
        must(
            "t_residual_castop_undermark_pending = false" in win,
            "AC5: test reset still clears all three persist TLS bits",
        )

    if failures:
        print(f"\ncheck_residual_castop_densify_clear_4155: {len(failures)} row(s) failed")
        for row in failures:
            print(f"  FAIL: {row}")
        return 1
    print("\ncheck_residual_castop_densify_clear_4155: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
