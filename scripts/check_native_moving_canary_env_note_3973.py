#!/usr/bin/env python3
# scripts/check_native_moving_canary_env_note_3973.py -- Issue #3973 gate.
#
# AC1: NativeMovingCanary notes THIS invoke's arena-tracked live ptrs — the
#      closure's captured env cell values (both the freeable and the heap
#      storage), not the stack token alone; the dispatch hands the invoke's
#      cid to the canary.
# AC2: the dtor unnotes exactly what the ctor noted (one shared cell walk,
#      same bridge).
# AC3: #3368 slot-XOR-canary — JIT env cells stay out of the slot family
#      (no register_external_root_slot_for_densify in the JIT TU).
# AC4: the stack token remains the #3857 entry-gate presence bit and the
#      arena entry gate still reads the process-wide inventory.
# AC5: not keyed on JIT table epoch; no invented g_3973_* counter; no
#      docs/design; no tests/issues file.
# AC6: test wiring — ac21_3973_native_canary_notes_env_cells wired into
#      run_test_setcode_rebind_survive after ac20, with a live dispatch
#      smoke across Moving postures.
# AC7: build.py registration + root_check_allowlist.txt append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

JIT = "src/compiler/aura_jit_runtime.cpp"
ARENA = "src/core/arena.ixx"
TEST = "tests/compiler/test_setcode_rebind_survive.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_native_moving_canary_env_note_3973"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(jit: str, arena: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — the canary notes the invoke's env cell values, not just the token.
    must("Issue #3973", "AC1 cite", jit)
    begin = jit.find("struct NativeMovingCanary")
    if begin < 0:
        fails.append("AC1: canary struct not located")
        return fails
    strct = jit[begin : begin + 3400]
    must("g_closure_is_arena", "AC1 freeable-storage walk", strct)
    must("g_arena_closure_env_sizes", "AC1 #1302 size bound", strct)
    must("g_arena_closure_envs", "AC1 freeable cells", strct)
    must("g_closure_envs", "AC1 heap cells", strct)
    must("walk_env_cells_", "AC1 shared cell walk", strct)
    must("aura_note_temporary_moving_live_ptr", "AC1 #3210 inventory bridge", strct)
    must(
        "NativeMovingCanary native_moving_canary{static_cast<size_t>(closure_id)}",
        "AC1 dispatch hands the invoke's cid",
        jit,
    )

    # AC2 — the dtor unnotes exactly what the ctor noted.
    must("aura_unnote_temporary_moving_live_ptr", "AC2 dtor unnote", strct)

    # AC3 — #3368 slot-XOR-canary: no slot registration in the JIT TU.
    must_not("register_external_root_slot_for_densify", "AC3 no slot dual-note", jit)

    # AC4 — stack token presence bit + arena entry gate intact.
    must("p(this)", "AC4 token presence bit", strct)
    must(
        "moving_temp_canary_detail::g_inventory.live",
        "AC4 arena entry gate reads the inventory",
        arena,
    )

    # AC5 — no second model, no invented counters, no docs, no issue file.
    must_not("g_closure_table_epochs", "AC5 not keyed on JIT table epoch", strct)
    must_not("g_3973_", "AC5 no invented counter", jit)
    for stale in ROOT.glob("docs/design/*3973*"):
        fails.append(f"AC5: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_3973*.cpp"):
        fails.append(f"AC5: forbidden issue test {stale.name}")

    # AC6 — test wiring after ac20 with a live dispatch smoke.
    must("ac21_3973_native_canary_notes_env_cells();", "AC6 runner wired", test)
    must("ac20_3972_native_dispatch_remap_arm();", "AC6 runner anchor", test)
    if test.find("ac21_3973_native_canary_notes_env_cells();") < test.find("ac20_3972_native_dispatch_remap_arm();"):
        fails.append("AC6: ac21 must run after ac20")
    must("--- #3973: native canary notes invoke env cells ---", "AC6 AC header", test)
    must("MovingFlagGuard on(1)", "AC6 Moving-on dispatch smoke", test)
    must("aura_closure_dispatch_native_checked(cid, args, 1)", "AC6 live dispatch", test)

    # AC7 — build.py registration + allowlist append.
    must("check_native_moving_canary_env_note_3973.py", "AC7 build.py registration", build)
    must(LINTER, "AC7 allowlist append", allow)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #3973 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    jit = _read(JIT)
    arena = _read(ARENA)
    test = _read(TEST)
    build = _read(BUILD)
    allow = _read(ALLOWLIST)
    if args.self_test:
        broken = jit.replace("walk_env_cells_", "walk_redacted_")
        self_fails = _rows(broken, arena, test, build, allow)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(jit, arena, test, build, allow)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
