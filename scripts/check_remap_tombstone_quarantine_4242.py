#!/usr/bin/env python3
"""Issue #4242: bind/resolve must not chase a #3469 tombstone onto a recycled
live address (apply UAF, P1 arena residual).

Hole: ASTArena::destroy recycles small-pool slots without touching
last_object_remap_, and #3469 keeps previous-window tombstone keys across
windows (A→B then B→C stays resolvable). When a recycled address A that is
STILL a tombstone key (A→B) is handed to a new tracked object Y, every
bind/resolve of the legitimately-live Y chases A onto B: the caller then
evaluates the wrong object (B still holds the old O) or a freed slot (B
destroyed). The TW apply stack-copy block assigns that chased return into
cl_copy.flat / cl_copy.pool, and production_apply_closure_densify_hard_refuse
runs only AFTER the rewrite and only refuses when the CHASED address is
itself a remap key — the alias slips through a green window.

Fix shape (one last_object_remap_; no second pin/GC table):
  - Quarantine: SmallObjectPool::try_allocate never returns a slot that is
    still a last_object_remap_ key. ASTArena installs a quarantine predicate
    (remap_tombstone_quarantine_pred_) at construction; the freelist walk
    pops and PARKS quarantined heads (freed at reset()) and a quarantined
    bump candidate fail-closes the tier. The #3469 fold only drops keys into
    states where the address is a live_new home or still tombstoned, so
    park-until-reset is the only safe release policy.
  - Guarded chase: resolve_object_remap_for_bind is the single chase SSOT —
    a tombstone may be followed only when the key is NOT a current tracked
    identity (dtor_index_) AND the destination is still tracked. A live key
    (test seam / same-window collision only) must not alias onto B; a dead
    destination must not be rewritten into the caller's copy — leaving the
    key address in place lets the #3421 refuse face still resolve it and
    refuse the apply, while quarantine keeps that address un-reused for
    Soft. bind_temporary_moving_live_ptr and the any-arena walk
    (resolve_ptr_for_chase) both go through it; resolve_object_remap (the
    refuse face) is unchanged.
  - Observability: g_moving_remap_tombstone_quarantine_total (declared
    before SmallObjectPool so its in-class bodies can bind it),
    g_moving_remap_bind_live_key_keep_total, g_moving_remap_bind_dead_dest_
    keep_total — append-only schema; Soft/Off never bump.
  - Soft/Off: empty table → predicate fast path (one empty() load); bind
    stays the one-load no-op when Moving is off.

Contract (one row per AC):
  AC1  arena.ixx quarantine: SmallObjectPool QuarantinePred hook installed
       by ASTArena ctor, freelist pop+park loop, bump fail-close, reset()
       frees parked slots, quarantine counter declared before the pool
       class, small_pool_quarantined_slot_count accessor
  AC2  guarded chase: resolve_object_remap_for_bind refuses live keys and
       dead destinations (bumps the matching counters), member bind and the
       any-arena walk (resolve_ptr_for_chase) use it, refuse-face
       resolve_object_remap unchanged (evaluator consult intact)
  AC3  one table only: exactly one std::unordered_map<void*, void*> member
       (last_object_remap_); no second pin/GC registry, no new query key
  AC4  runtime ACs dispatched in tests/core/test_moving_densify_fail_closed.cpp
       (ac4242_1..ac4242_5 in run_test_moving_densify_fail_closed before the
       Results line); no tests/**/test_issue_4242.cpp; no docs/design/4242-*
  AC5  build.py wiring + scripts/coverage/root_check_allowlist.txt entry

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    arena = _read("src/core/arena.ixx")
    apply_src = _read("src/compiler/evaluator_eval_flat.cpp")
    test_src = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build_py = _read("build.py")
    allowlist = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: quarantine (try_allocate never returns a tombstone-keyed slot).
    must("Issue #4242", "AC1 arena cites #4242", arena)
    must("using QuarantinePred = bool (*)(void* ctx, void* candidate) noexcept", "AC1 QuarantinePred alias", arena)
    must("void set_quarantine_pred(QuarantinePred pred, void* ctx) noexcept", "AC1 pool quarantine hook setter", arena)
    must("quarantined_.push_back(parked)", "AC1 freelist pop+park", arena)
    must("quarantine_pred_(quarantine_ctx_, ptr)", "AC1 bump fail-close probe", arena)
    must("quarantined_.clear(); // Issue #4242: tombstones clear with the table", "AC1 reset frees parked slots", arena)
    must(
        "export inline std::atomic<std::uint64_t> g_moving_remap_tombstone_quarantine_total{0};",
        "AC1 quarantine counter",
        arena,
    )
    must(
        "small_pool_.set_quarantine_pred(&ASTArena::remap_tombstone_quarantine_pred_, this)",
        "AC1 ctor installs the predicate",
        arena,
    )
    must("self->last_object_remap_.contains(candidate)", "AC1 predicate keys on last_object_remap_", arena)
    must(
        "[[nodiscard]] std::size_t small_pool_quarantined_slot_count() const noexcept",
        "AC1 parked-slot accessor",
        arena,
    )
    # Declaration order: the counter must precede the pool class so the
    # in-class try_allocate body can bind it (C++ member lookup rule).
    counter_pos = arena.find("g_moving_remap_tombstone_quarantine_total{0}")
    pool_pos = arena.find("export class SmallObjectPool")
    if counter_pos == -1 or pool_pos == -1 or counter_pos > pool_pos:
        fails.append("AC1: quarantine counter must be declared before SmallObjectPool")

    # ── AC2: guarded chase (live-key / dead-destination refusals).
    must(
        "[[nodiscard]] void* resolve_object_remap_for_bind(void* old_ptr) const noexcept",
        "AC2 guarded-chase SSOT",
        arena,
    )
    must(
        "g_moving_remap_bind_live_key_keep_total.fetch_add(1, std::memory_order_relaxed)",
        "AC2 live-key keep counter bump",
        arena,
    )
    must(
        "g_moving_remap_bind_dead_dest_keep_total.fetch_add(1, std::memory_order_relaxed)",
        "AC2 dead-destination keep counter bump",
        arena,
    )
    must("if (dtor_index_.contains(old_ptr))", "AC2 live-key arm reads dtor_index_", arena)
    must("if (!dtor_index_.contains(neu))", "AC2 dead-destination arm reads dtor_index_", arena)
    must(
        "if (void* neu = resolve_object_remap_for_bind(p))\n            p = neu;",
        "AC2 member bind goes through the guard",
        arena,
    )
    must("inline void* resolve_ptr_for_chase(void* p) noexcept", "AC2 any-arena chase walk", arena)
    must(
        "if (void* neu = live_arena_remap_detail::resolve_ptr_for_chase(p))",
        "AC2 any-arena bind goes through the guard",
        arena,
    )
    # The refuse face is unchanged: the TW refuse helper still consults the
    # raw table so a refused chase (key kept in place) still refuse the apply.
    must(
        "if (cl.flat && ar->resolve_object_remap(static_cast<void*>(cl.flat)))",
        "AC2 refuse face still resolves the raw key",
        apply_src,
    )
    must(
        "if (cl.pool && ar->resolve_object_remap(static_cast<void*>(cl.pool)))",
        "AC2 refuse face covers pool copies",
        apply_src,
    )
    must(
        "export inline std::atomic<std::uint64_t> g_moving_remap_bind_live_key_keep_total{0};",
        "AC2 live-key counter declared",
        arena,
    )
    must(
        "export inline std::atomic<std::uint64_t> g_moving_remap_bind_dead_dest_keep_total{0};",
        "AC2 dead-dest counter declared",
        arena,
    )

    # ── AC3: one table only — quarantine is a predicate, not a registry.
    # Member-style declarations only (name ends with the member underscore,
    # no reference decoration): params and the #3781 this_window_remap local
    # share the type spelling but are not stored registries.
    member_maps = [
        ln.strip()
        for ln in arena.splitlines()
        if ln.strip().startswith("std::unordered_map<void*, void*>")
        and ln.strip().endswith(";")
        and "&" not in ln
        and ln.strip().split()[-1].rstrip(";").endswith("_")
    ]
    if len(member_maps) != 1 or "last_object_remap_;" not in member_maps[0]:
        fails.append(f"AC3: exactly one void*→void* member map allowed (last_object_remap_), got {member_maps!r}")
    absent("remap_quarantine_map", "AC3 no second remap registry", arena)
    absent("g_4242_", "AC3 no issue-numbered query key", arena)
    must("inline constexpr int kMovingRemapTombstoneQuarantineIssue = 4242;", "AC3 issue constant pinned", arena)

    # ── AC4: runtime ACs dispatched; no new test file / design doc.
    for fn in (
        "static void ac4242_1_quarantine_recycle_never_lands_on_key()",
        "static void ac4242_2_bind_live_key_guard()",
        "static void ac4242_3_dead_dest_never_rewritten()",
        "static void ac4242_4_multi_window_refuse_retained()",
        "static void ac4242_5_soft_off_noop_and_source()",
    ):
        must(fn, "AC4 test definition", test_src)
    for call in (
        "ac4242_1_quarantine_recycle_never_lands_on_key();",
        "ac4242_2_bind_live_key_guard();",
        "ac4242_3_dead_dest_never_rewritten();",
        "ac4242_4_multi_window_refuse_retained();",
        "ac4242_5_soft_off_noop_and_source();",
    ):
        must(call, "AC4 dispatcher call", test_src)
    # Calls must run before the Results line so a failure flips the tally.
    results_pos = test_src.find("=== Results: {} passed, {} failed ===")
    last_call_pos = test_src.find(
        "ac4242_5_soft_off_noop_and_source();", test_src.find("int run_test_moving_densify_fail_closed()")
    )
    if results_pos == -1 or last_call_pos == -1 or last_call_pos > results_pos:
        fails.append("AC4: ac4242_* calls must precede the Results line")
    must("--- #4242 AC1", "AC4 runtime header", test_src)
    if _read("tests/core/test_issue_4242.cpp"):
        fails.append("AC4: tests/core/test_issue_4242.cpp must not exist")
    if _read("docs/design/4242-remap-tombstone-quarantine.md"):
        fails.append("AC4: docs/design/4242-* must not exist")

    # ── AC5: gate wiring.
    must("check_remap_tombstone_quarantine_4242.py", "AC5 build.py wiring", build_py)
    must("check_remap_tombstone_quarantine_4242.py", "AC5 allowlist entry", allowlist)

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("ok #4242 remap tombstone quarantine: 5/5 contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
