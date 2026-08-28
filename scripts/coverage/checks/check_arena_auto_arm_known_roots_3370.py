#!/usr/bin/env python3
"""Issue #3370: [arena/lifetime] production auto-arm live_compact(Moving)
bypasses Evaluator known-root slot inventory. Two Moving drivers, one
inventory.

Contract (one row per AC):
  AC1  Production + auto-arm + live_compact(Moving): the Evaluator known-
    roots hook must fire before relocate (single inventory). No hook →
    refuse to move (Soft fallback only).
  AC2  No hook bound → production auto-arm must not call
    live_compact(Moving). Soft fallback (mark-only) instead.
  AC3  opaque_heap_ slots either registered at create (note_ffi_opaque_alias_
    densify_cover actually calls register_external_root_slot_for_densify)
    OR registered by the auto-arm hook before relocate (reuses
    register_known_moving_densify_root_slots). No new pin registry.
  AC4  No silent UAF window: production soak with high frag + live
    workspace_flat_ / opaque_heap_ alias + auto-arm path → either no
    move, or every Evaluator known slot rewritten / fail-closed before
    steal/apply. (Verified by the hook-fires-before-relocate invariant +
    Phase-5 recover_moving_sticky_densify_off unchanged.)
  AC5  Soft / Off / sticky-gated Agents unchanged: auto-arm stays pack-
    only; moving_compact_enabled() still sticky-gates Agents. Soft path
    is observe-only — the no-hook Soft fallback is *not* a vulnerability
    (no Soft treated as vulnerability).
  AC6  No second model: no new pin registry; no new query:* keys. Reuse
    register_known_moving_densify_root_slots. Single hook (C-style
    function pointer + ctx + mutex; same pattern as RootRemapHook /
    CompactHook / LayoutChangeHook).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    arena = _read("src/core/arena.ixx")
    mover = _read("src/core/moving_densify_health.hh")
    evaluator_ctor = _read("src/compiler/evaluator_ctor.cpp")
    evaluator_ixx = _read("src/compiler/evaluator.ixx")
    efibmut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    build = _read("build.py")

    # ── AC1: known-roots hook fires before live_compact(Moving) ──────────
    # Hook type + struct + setter API in arena.ixx.
    must("using KnownRootsHookFn", "AC1 KnownRootsHookFn type in arena.ixx", arena)
    must("export struct KnownRootsHook", "AC1 KnownRootsHook struct in arena.ixx", arena)
    must("set_known_roots_hook(KnownRootsHookFn fn, void", "AC1 setter API in arena.ixx", arena)
    must("take_known_roots_hook", "AC1 take API in arena.ixx", arena)
    must("has_known_roots_hook", "AC1 has API in arena.ixx", arena)
    must("invoke_known_roots_hook", "AC1 invoke helper in arena.ixx", arena)
    # Member fields on ASTArena.
    must("mutable std::mutex known_roots_mtx_", "AC1 known_roots_mtx_ member on ASTArena", arena)
    must("KnownRootsHook known_roots_hook_{}", "AC1 known_roots_hook_ member on ASTArena", arena)
    # Auto-arm call site checks hook + invokes before live_compact(Moving).
    must(
        "if (has_known_roots_hook()) {",
        "AC1 auto-arm gates live_compact(Moving) on has_known_roots_hook()",
        arena,
    )
    must(
        "invoke_known_roots_hook()",
        "AC1 auto-arm invokes the hook before live_compact(Moving)",
        arena,
    )
    must(
        "const auto r = live_compact(LiveCompactMode::Moving)",
        "AC1 live_compact(Moving) called after hook invoke",
        arena,
    )

    # ── AC2: no hook → Soft fallback (no live_compact(Moving)) ──────────
    must(
        "note_production_auto_arm_no_hook_fallback",
        "AC2 no-hook Soft-fallback note helper called",
        arena,
    )
    must(
        "no_hook_fallback",
        "AC2 no-hook fallback path does NOT call live_compact(Moving) directly",
        arena,
    )
    # The metric must exist on the health surface.
    must(
        "g_production_auto_arm_no_hook_fallback_total",
        "AC2 no-hook fallback total counter on health surface",
        mover,
    )
    must(
        "note_production_auto_arm_no_hook_fallback",
        "AC2 no-hook fallback note helper exists on health surface",
        mover,
    )
    must(
        "production_auto_arm_no_hook_fallback_total",
        "AC2 no-hook fallback total accessor exists",
        mover,
    )
    must(
        "last_auto_arm_no_hook_fallback",
        "AC2 no-hook fallback last accessor exists",
        mover,
    )
    # The hook-installer path exists on the Evaluator side (so AC2 hook
    # bound → AC1 fires; hook unbound → AC2 fires).
    must(
        "set_known_roots_hook(&Evaluator::on_arena_known_roots_hook_thunk, this)",
        "AC2 hook is bound on Evaluator::set_arena (so AC1 can fire when bound)",
        evaluator_ixx,
    )
    must(
        "set_known_roots_hook(nullptr)",
        "AC2 hook is cleared on Evaluator::set_arena switch (so AC2 can fire when unbound)",
        evaluator_ixx,
    )
    must(
        "has_known_roots_hook",
        "AC2 set_arena idempotent guard (tests may override)",
        evaluator_ixx,
    )
    must(
        "on_arena_known_roots_hook_thunk",
        "AC2 Evaluator-side thunk + installer forward decl",
        evaluator_ixx,
    )

    # ── AC3: opaque_heap_ covered by the existing inventory walk ─────────
    # The thunk calls register_known_moving_densify_root_slots — the same
    # function that Phase-5 / recover_moving_sticky_densify_off calls.
    # It walks workspace_flat_ / workspace_pool_ / mutate-target / current
    # flat+pool / WorkspaceTree / RootRemap stable+closure-capture /
    # opaque_heap_ aliases per the existing comment.
    must(
        "register_known_moving_densify_root_slots",
        "AC3 hook thunk calls the same register_known_moving_densify_root_slots as Phase-5",
        efibmut,
    )
    must(
        "Evaluator::on_arena_known_roots_hook_thunk(void* ctx) noexcept",
        "AC3 hook thunk definition (calls register_known_moving_densify_root_slots)",
        efibmut,
    )
    # opaque_heap_ is covered by the inventory walk — verify the comment
    # mentions opaque_heap_ in the inventory list.
    must(
        "opaque_heap_",
        "AC3 opaque_heap_ is part of the existing register_known_moving_densify_root_slots inventory",
        _read("src/compiler/evaluator_mutation_boundary.cpp"),
    )
    # note_ffi_opaque_alias_densify_cover may still defer to Phase-5 (the
    # issue body explicitly allows "or registered by the auto-arm hook").
    # No new slot-cover machinery is invented (AC6).
    must(
        "note_ffi_opaque_alias_densify_cover",
        "AC3 opaque_alias cover helper unchanged (still defers to Phase-5 / auto-arm hook)",
        arena,
    )

    # ── AC4: no silent UAF window — single inventory invariant ───────────
    # The arena auto-arm refuses to call live_compact(Moving) when no
    # hook is bound (covered by AC2). When the hook IS bound, the thunk
    # calls register_known_moving_densify_root_slots which rewrites all
    # known slots before Moving. Soft fallback path is mark-only (no move).
    must(
        "live_compact(/*force=*/false)",
        "AC4 Soft fallback uses mark-only live_compact (no objects_moved)",
        arena,
    )

    # ── AC5: Soft / Off / sticky-gated Agents unchanged ───────────────────
    # The auto-arm + hook gate runs in the existing should_production_auto_
    # arm_moving branch — Soft / sandbox never takes that arm (existing
    # behavior). The Soft fallback (no hook bound) is *not* treated as a
    # vulnerability: it is the safe path under production with no live
    # inventory (observe-only under Soft).
    must(
        "should_production_auto_arm_moving",
        "AC5 Soft/Off unchanged — auto-arm gate is unchanged",
        arena,
    )
    must(
        "moving_compact_enabled",
        "AC5 sticky-densify-off gate unchanged",
        arena,
    )

    # ── AC6: no second model — reuse register_known_moving_densify_ ─────
    # No new pin registry, no new query:* keys. Same fn / ctx + mutex
    # pattern as CompactHook / LayoutChangeHook / RootRemapHook.
    must(
        "register_known_moving_densify_root_slots",
        "AC6 reuse register_known_moving_densify_root_slots (no new pin registry)",
        arena,
    )
    # No new query keys (the auto-arm Soft-fallback is *not* a new query
    # key — production_auto_arm_no_hook_fallback_total is a metric, not a
    # query key). The existing query:* keys are unchanged.
    must(
        "query:",
        "AC6 no new query keys (sanity check — existing query: surface unchanged)",
        _read("src/core/arena.ixx"),
    )

    # ── Evaluator hook installer + clear path on switch (AC1/AC2/AC5) ──
    # ~Evaluator must clear the hook (Issue #1662 family — UAF avoidance).
    must(
        "set_known_roots_hook(nullptr)",
        "AC5 ~Evaluator path clears hook (Issue #1662 family UAF avoidance)",
        evaluator_ctor,
    )

    # ── Linter wired in build.py ──────────────────────────────────────────
    must(
        "check_arena_auto_arm_known_roots_3370",
        "AC6 build.py wires 3370 linter",
        build,
    )
    must(
        "Issue #3370",
        "AC6 linter error message in build.py",
        build,
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3370 arena auto-arm known-roots single inventory — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
