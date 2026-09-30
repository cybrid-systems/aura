#!/usr/bin/env python3
"""Issue #4244: c-struct-set! int/float interior can hold an arena pointer
with no slot remap (UAF / wrong object on the native read, P1 arena residual).

Hole: the opaque arm of c-struct-set! memcpy's then joins the cover triad
(opaque_heap_element_cover_or_required_fail + #4068 durable interior-slot
re-arm), but the int/float arms memcpy raw pointer-sized bits into the libc
buffer and never register the interior word. Create-time
note_ffi_opaque_create_exempt("libc-heap") on c-alloc is not remap cover for
bytes stored later: under Moving, a stored pattern that IS a live arena
address stays densify-old after the next window (the word is not a
last_object_remap_ key — it is not registered anywhere), and the native
c-func read dereferences the stale address. The opaque interior arm landed
the exact machinery; the int arm is the bypass.

Fix shape (same helper as the opaque arm — no second pin registry):
  - Query: interior_int_pattern_is_live_arena_object(pattern) resolves the
    stored pointer-sized pattern against every live arena — last_object_remap_
    key (relocated >= once, #3469 tombstones kept) OR dtor_index_ (live
    tracked, never moved; new ASTArena::tracks_live_object + the
    live_arena_remap_detail::live_tracked walk). Off / !moving_compact_enabled:
    one load, false — the memcpy face keeps zero extra remap.
  - Cover: cover_interior_int_pattern_for_densify(base+offset slot, pattern)
    registers the stable libc interior word with the SAME durable inventory
    via register_struct_interior_slot_for_densify (#4068 re-arm) so the next
    window's rewrite walk rewrites *slot via this_window_remap, and bumps
    g_ffi_interior_int_slot_cover_total. A non-arena pattern stays a plain
    memcpy (no register, no counter, no canary) in every mode.
  - Wiring: both the int and the float arm of c-struct-set! call the cover
    helper behind a sizeof(v) == sizeof(void*) guard; the opaque arm is
    untouched.
  - Soft/Off: query early-returns (one load); the pinned non-arena int stays
    a plain memcpy (no pin, no canary, no register).

Contract (one row per AC):
  AC1  arena.ixx owns the helper set: ASTArena::tracks_live_object (dtor_index_
       oracle), live_arena_remap_detail::live_tracked (any-arena walk),
       exported interior_int_pattern_is_live_arena_object (moving gate),
       exported cover_interior_int_pattern_for_densify reusing
       register_struct_interior_slot_for_densify (same durable inventory)
       + counter bump
  AC2  ffi c-struct-set!: int arm AND float arm call
       cover_interior_int_pattern_for_densify behind the pointer-sized guard
       inside the prim body; the opaque arm's cover triad untouched
  AC3  one inventory only: counter declared once in
       densify_consistency_report.h (g_ffi_interior_int_slot_cover_total),
       no invented pin/registry class, no docs/design/4244-*
  AC4  runtime ACs dispatched in tests/core/test_moving_densify_fail_closed.cpp
       (ac4244_1..ac4244_3 in run_test_moving_densify_fail_closed before the
       Results line); no tests/**/test_issue_4244.cpp
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

    # AC1: arena.ixx helper set over the EXISTING tables/inventory.
    must("bool tracks_live_object(void* p) const noexcept", "AC1 member oracle", arena)
    must("Issue #4244: live-tracked oracle", "AC1 member cite", arena)
    must("inline bool live_tracked(void* p) noexcept", "AC1 any-arena walk", arena)
    must("arena->tracks_live_object(p)", "AC1 walk uses the member", arena)
    must(
        "export inline bool interior_int_pattern_is_live_arena_object(",
        "AC1 query helper",
        arena,
    )
    must(
        "export inline bool cover_interior_int_pattern_for_densify(void** slot,",
        "AC1 cover helper",
        arena,
    )
    must("if (!moving_compact_enabled())", "AC1 moving gate", arena)
    must(
        "live_arena_remap_detail::resolves(p) || live_arena_remap_detail::live_tracked(p)",
        "AC1 both resolve arms",
        arena,
    )
    must("register_struct_interior_slot_for_densify(slot);", "AC1 #4068 re-arm reuse", arena)
    must(
        "aura::core::densify_consistency::g_ffi_interior_int_slot_cover_total.fetch_add(",
        "AC1 counter bump",
        arena,
    )
    absent("class InteriorIntRegistry", "AC1 no invented registry (arena)", arena)
    absent("class InteriorPinRegistry", "AC1 no second pin table (arena)", arena)

    # AC2: ffi prim arms call the cover helper; opaque arm untouched.
    ffi = _read("src/compiler/ffi_primitives_impl.cpp")
    setb = ffi.find('add("c-struct-set!"')
    sete = ffi.find('add("c-struct-ref"', setb)
    if setb == -1 or sete == -1 or sete <= setb:
        fails.append("AC2 prim body: c-struct-set! body not bounded")
        body = ""
    else:
        body = ffi[setb:sete]
    if body:
        uses = body.count("cover_interior_int_pattern_for_densify")
        if uses < 2:
            fails.append(f"AC2 prim body: cover helper called {uses}x, need int+float (2)")
        must("if constexpr (sizeof(v) == sizeof(void*))", "AC2 pointer-sized guard", body)
        must("Issue #4244", "AC2 prim cites", body)
        must("opaque_heap_element_cover_or_required_fail(ptr, interior,", "AC2 opaque arm intact", body)
        must("register_struct_interior_slot_for_densify(interior);", "AC2 opaque re-arm intact", body)

    # AC3: one inventory only — counter home + no invented tables/docs.
    rep = _read("src/core/densify_consistency_report.h")
    must(
        "inline std::atomic<std::uint64_t> g_ffi_interior_int_slot_cover_total{0};",
        "AC3 counter declared",
        rep,
    )
    must(
        "inline void reset_ffi_interior_int_slot_cover_for_test() noexcept",
        "AC3 counter test reset",
        rep,
    )
    if rep.count("g_ffi_interior_int_slot_cover_total{0}") != 1:
        fails.append("AC3 one inventory: counter declared more than once")
    absent("class InteriorIntPinRegistry", "AC3 no second registry (ffi)", ffi)
    must("", "AC3 no docs/design/4244-*", _read("docs/design/4244-interior-int-remap.md"))

    # AC4: runtime ACs dispatched in the existing batch member.
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    for fn in (
        "ac4244_1_int_float_pattern_cover_rewrites_on_moving();",
        "ac4244_2_resolve_oracle_tombstone_and_non_arena();",
        "ac4244_3_soft_off_and_source();",
    ):
        must(fn, "AC4 dispatch", test)
    runb = test.find("int run_test_moving_densify_fail_closed()")
    res = test.find("=== Results:", runb)
    if runb == -1 or res == -1:
        fails.append("AC4 dispatch: runner / Results line not found")
    else:
        for fn in ("ac4244_1_", "ac4244_2_", "ac4244_3_"):
            seg = test[runb:res]
            if fn not in seg:
                fails.append(f"AC4 dispatch: {fn} not before the Results line")
    must("", "AC4 no test_issue_4244.cpp", _read("tests/core/test_issue_4244.cpp"))
    must("#4244 AC1", "AC4 AC1 body", test)
    must("#4244 AC2", "AC4 AC2 body", test)
    must("#4244 AC3", "AC4 AC3 body", test)

    # AC5: build.py wiring + root allowlist entry.
    build = _read("build.py")
    must("check_interior_int_remap_4244.py", "AC5 build.py wiring", build)
    allow = _read("scripts/coverage/root_check_allowlist.txt")
    must("check_interior_int_remap_4244.py", "AC5 allowlist entry", allow)

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("check_interior_int_remap_4244: OK (5 AC rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
