#!/usr/bin/env python3
"""Issue #3848: densify-stale refuse must not disarm on objects_moved==0.

Residual: production_apply_closure_densify_hard_refuse returned false early
when g_last_objects_moved==0, before window/LCP/remap. Zero-move publishes
leave densify-old tombstones → UAF if refuse is skipped.

Contract (one row per AC):
  AC1  Remove objects_moved==0 early return; order production → window →
       empty-remap fast-path → LCP → resolve_object_remap(flat|pool);
       Soft/Off never refuse; both closure + FFI arms; no second pin registry
  AC2  Extends densify-stale (test_setcode_rebind_survive ac16) + moving
       densify fail-closed (ac3848_*); Soft empty-remap recover retained
  AC3  Dedicated linter + grandfather + manifest + build.py; no invent /
       docs/design (#1655 / #81967)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FLAT = "src/compiler/evaluator_eval_flat.cpp"
SURVIVE = "tests/compiler/test_setcode_rebind_survive.cpp"
FAIL = "tests/core/test_moving_densify_fail_closed.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3848.json"
LINTER = "check_densify_refuse_zero_move_3848"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    flat = _read(FLAT)
    survive = _read(SURVIVE)
    failc = _read(FAIL)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    begin = flat.find("static bool production_apply_closure_densify_hard_refuse")
    end = flat.find("static void note_apply_closure_densify_hard_refuse", begin)
    if begin < 0 or end <= begin:
        fails.append("AC1: closure arm not located")
        arm = ""
    else:
        arm = flat[begin:end]
    must("Issue #3848", "AC1 cite", flat)
    must("window_would_allow_mutate", "AC1 window gate", arm)
    must("object_remap_size", "AC1 empty-remap fast-path", arm)
    must("last_lifetime_consistency_would_allow", "AC1 LCP", arm)
    must("resolve_object_remap(static_cast<void*>(cl.flat))", "AC1 flat remap", arm)
    must("resolve_object_remap(static_cast<void*>(cl.pool))", "AC1 pool remap", arm)
    must("Soft / Off never take this refuse", "AC1 Soft contract", flat)
    must_not("g_last_objects_moved", "AC1 no moved early return", arm)
    must_not("g_3848_", "AC1 no invented counter", flat)
    must_not("class DensifyClosurePinRegistry", "AC1 no second pin registry", flat)
    must_not("g_moving_pin_registry_3848", "AC1 no second pin registry", flat)
    if arm:
        gate = arm.find("window_would_allow_mutate")
        remap_sz = arm.find("object_remap_size")
        lcp = arm.find("last_lifetime_consistency_would_allow")
        if not (0 <= gate < remap_sz < lcp):
            fails.append("AC1: order must be window → empty-remap → LCP")

    ffi_begin = flat.find("production_ffi_apply_densify_hard_refuse")
    ffi_end = flat.find("// Issue #1511", ffi_begin)
    if ffi_begin < 0 or ffi_end <= ffi_begin:
        fails.append("AC1: FFI arm not located")
    else:
        ffi = flat[ffi_begin:ffi_end]
        must("Issue #3848", "AC1 FFI cite", ffi)
        must("window_would_allow_mutate", "AC1 FFI window", ffi)
        must("object_remap_size", "AC1 FFI empty-remap", ffi)
        must_not("g_last_objects_moved", "AC1 FFI no moved early return", ffi)

    must("ac16_3848_zero_move_publish_still_refuse();", "AC2 densify-stale suite", survive)
    must("zero-move publish still refuses densify-old flat*", "AC2 refuse AC", survive)
    must("ac3848_1_source_cite_no_moved_early_return();", "AC2 fail_closed suite", failc)
    must("ac3848_2_zero_move_publish_keeps_tombstones();", "AC2 tombstone AC", failc)
    must("production + empty remap still recover", "AC2 Soft/empty recover", survive)

    must(LINTER, "AC3 build registration", build)
    must("Issue #3848", "AC3 build cite", build)
    must(f"{LINTER}.py", "AC3 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC3 grandfather path", gf)
    must('"issue": 3848', "AC3 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC3: {MANIFEST} missing")
    for rel in (
        "tests/compiler/test_issue_3848.cpp",
        "tests/core/test_issue_3848.cpp",
        "tests/issues/test_issue_3848.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC3: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3848*"):
            fails.append(f"AC3: docs/design/{p.name} exists")

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3848", file=sys.stderr)
        return 1

    print(f"OK {LINTER}: densify refuse survives zero-move publish")
    return 0


if __name__ == "__main__":
    sys.exit(main())
