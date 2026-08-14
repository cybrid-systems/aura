#!/usr/bin/env python3
"""Issue #3022: FFI opaque/native buffer pin-or-remap (or EXEMPT).

Contract (one row per AC):
  AC1  FFI handoff create sites (c-opaque, c-alloc, c-struct-ref opaque,
       apply_closure Opaque return, ffi:pin-buffer) are pin / slot /
       EXEMPT. No create-point observe-only.
  AC2  Production required + Moving: uncovered still fail-closed via
       existing breach / untracked / live_pin_count. FfiOwned blocks
       reclaim (any_pin_blocks_arena_reclaim).
  AC3  No second pin registry. Soft extra cost is exempt counter, no pin.
  AC4  Tests: pin-required fail-closed + FFI densify soak + FfiOwned
       reclaim canary in test_general_object_pin. No test_issue_3022.cpp.
       No docs/design/ (#1655).

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

    lp = _read("src/core/lifetime_pin.hh")
    ixx = _read("src/core/lifetime_pin.ixx")
    ffi = _read("src/compiler/ffi_primitives_impl.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_general_object_pin.cpp")
    build = _read("build.py")

    # ── AC1: inventory ──
    must("kFfiOpaquePinOrRemapIssue", "AC1 stamp", lp)
    must("note_ffi_opaque_create_exempt", "AC1 helper", lp)
    must("GENERAL_OBJECT_PIN_EXEMPT: external-native-addr", "AC1 c-opaque", ffi)
    must("GENERAL_OBJECT_PIN_EXEMPT: libc-heap", "AC1 c-alloc", ffi)
    must("GENERAL_OBJECT_PIN_EXEMPT: opaque-struct-copy", "AC1 struct-ref", ffi)
    must("GENERAL_OBJECT_PIN_EXEMPT: ffi-return-external", "AC1 apply Opaque", flat)
    must("mark_ffi_owned", "AC1 pin-buffer owned", ffi)
    must("note_ffi_opaque_create_exempt", "AC1 ffi calls helper", ffi)
    if "AgentRegistry" in ffi:
        fails.append("AC1: must not introduce AgentRegistry")

    # ── AC2: fail-closed + FfiOwned ──
    must("any_pin_blocks_arena_reclaim", "AC2 reclaim walk", lp)
    must("blocks_arena_reclaim", "AC2 FfiOwned", lp)
    must("g_general_object_pin_required_breach", "AC2 reuse breach", lp)
    must("AC3022: FfiOwned blocks_arena_reclaim", "AC2 canary", test)
    must("AC3022: Force blocked while FfiOwned", "AC2 Force", test)

    # ── AC3: no second registry / Soft ──
    must("no second pin registry", "AC3", lp)
    must("any_pin_blocks_arena_reclaim", "AC3 export", ixx)
    must("note_ffi_opaque_create_exempt", "AC3 export", ixx)
    must("zero extra pin", "AC3 Soft", lp)
    must("schema-3022", "AC3 query", obs)
    must("ffi-opaque-pin-or-remap-wired", "AC3 wired", obs)

    # ── AC4: tests + wiring ──
    must("ac3022_ffi_handoff_inventory", "AC4 inventory test", test)
    must("ac3022_ffi_owned_blocks_reclaim", "AC4 canary test", test)
    must("ac3022_pin_required_and_soak", "AC4 soak test", test)
    must("check_ffi_opaque_pin_or_remap_3022", "AC4 build", build)
    must("cmd_ffi_opaque_pin_or_remap_3022", "AC4 build cmd", build)
    for rel in (
        "tests/compiler/test_issue_3022.cpp",
        "tests/core/test_issue_3022.cpp",
    ):
        if _read(rel):
            fails.append(f"AC4: {rel} exists — forbidden per #81967")
    if _read("docs/design/3022-ffi-opaque-pin-or-remap.md"):
        fails.append("AC4: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3022 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3022 FFI opaque pin-or-remap — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
