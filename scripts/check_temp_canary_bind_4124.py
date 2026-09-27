#!/usr/bin/env python3
# scripts/check_temp_canary_bind_4124.py -- Issue #4124 gate.
#
# AC1: JIT NativeMovingCanary env cells note through the #4066 bind
#      contract — the extern "C" any-arena bind bridge (inventory mutex +
#      last_object_remap_ resolve + note) — and the dtor unnotes the stored
#      BOUND addresses (bound_inline / bound_spill), not the densify-old
#      cell values; the #3857 presence bit is untouched.
# AC2: TemporaryMovingLivePtrCanary::arm_observe is Soft-only: the gate is
#      inverted (returns when moving_compact_enabled()) and the #4124
#      rationale is documented next to it.
# AC3: bind_temporary_moving_live_ptr_any_arena exists in arena.ixx with the
#      identical mutex+remap+note contract (resolve through
#      live_arena_remap_detail::resolve_ptr, waiter instrumentation, bound
#      return) plus the extern "C" bridge for the JIT TU; no invented
#      counter / second registry.
# AC4: the TW apply null-arena fallback binds (own_noted) — no raw
#      arm_observe note in the apply path.
# AC5: test wiring — the four ac4124_* functions run after the #4066 block;
#      the adversarial window test calls the any-arena bind; no
#      docs/design/4124-*; no tests/**/test_issue_4124.cpp.
# AC6: build.py registration + root_check_allowlist.txt append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

JIT = "src/compiler/aura_jit_runtime.cpp"
ARENA = "src/core/arena.ixx"
APPLY = "src/compiler/evaluator_eval_flat.cpp"
TEST = "tests/core/test_moving_densify_fail_closed.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_temp_canary_bind_4124"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(jit: str, arena: str, apply: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — JIT env cells bind; dtor unnotes the stored bound addresses.
    begin = jit.find("struct NativeMovingCanary")
    if begin < 0:
        fails.append("AC1: canary struct not located")
        return fails
    strct = jit[begin : begin + 5600]
    must("aura_bind_temporary_moving_live_ptr_any_arena", "AC1 bind bridge on cells", strct)
    must("bound_inline", "AC1 bound inline storage", strct)
    must("bound_spill", "AC1 bound spill storage", strct)
    must("walk_env_cells_", "AC1 shared cell walk", strct)
    must("aura_note_temporary_moving_live_ptr(p)", "AC1 #3857 presence bit intact", strct)
    must("aura_unnote_temporary_moving_live_ptr", "AC1 dtor unnote", strct)
    must_not(
        "aura_note_temporary_moving_live_ptr(v)",
        "AC1 no raw cell-value note",
        strct,
    )
    must(
        'extern "C" void* aura_bind_temporary_moving_live_ptr_any_arena(void* p) noexcept;',
        "AC1 bridge declared for the JIT TU",
        jit,
    )
    must(
        "NativeMovingCanary native_moving_canary{static_cast<size_t>(closure_id)}",
        "AC1 dispatch hands the invoke's cid",
        jit,
    )

    # AC2 — arm_observe is Soft-only (inverted gate, documented).
    arm = arena.find("void arm_observe(void* p) noexcept")
    if arm < 0:
        fails.append("AC2: arm_observe not located")
        return fails
    arm_span = arena[max(0, arm - 1000) : arm + 400]
    must("Issue #4124", "AC2 gate rationale documented", arm_span)
    must_not("!moving_compact_enabled()", "AC2 old gate removed", arm_span)
    must("moving_compact_enabled()", "AC2 Soft-only gate present", arm_span)

    # AC3 — the any-arena bind contract in arena.ixx.
    must("Issue #4124: bind contract for TUs that never hold an ASTArena*", "AC3 cite", arena)
    must(
        "export inline void* bind_temporary_moving_live_ptr_any_arena(void* p,",
        "AC3 bind contract exists",
        arena,
    )
    must("live_arena_remap_detail::resolve_ptr", "AC3 resolve across live arenas", arena)
    must("g_moving_canary_lock_waiters.fetch_add", "AC3 waiter instrumentation", arena)
    must("inv.ptrs.push_back(p)", "AC3 note under the inventory mutex", arena)
    must("return noted ? bound : nullptr;", "AC3 bridge bound-return", arena)
    must_not("g_4124_", "AC3 no invented counter", arena)
    must_not("class PostMovingCanaryRegistry", "AC3 no second registry", arena)

    # AC4 — TW apply fallback binds; no raw arm_observe note in the path.
    must(
        "aura::ast::bind_temporary_moving_live_ptr_any_arena(cl_copy.flat, &noted)",
        "AC4 apply binds flat via any-arena",
        apply,
    )
    must(
        "aura::ast::bind_temporary_moving_live_ptr_any_arena(cl_copy.pool, &noted)",
        "AC4 apply binds pool via any-arena",
        apply,
    )
    must("tmp_flat.own_noted(flat)", "AC4 #4066 own_noted contract intact", apply)
    must_not("tmp_flat.arm_observe(cl_copy.flat)", "AC4 no raw fallback note", apply)

    # AC5 — test wiring after the #4066 block + adversarial window test.
    must("ac4124_bind_any_blocks_then_remapped();", "AC5 runner wired", test)
    must("ac4124_bind_any_before_window_soft_gates();", "AC5 pre-window soft-gate wired", test)
    must("ac4124_arm_observe_soft_only();", "AC5 arm_observe AC wired", test)
    must("ac4124_soft_and_source();", "AC5 source-cite AC wired", test)
    must(
        "=== Issue #4124: temp canary must bind (remap+note under mutex) ===",
        "AC5 runner header",
        test,
    )
    must(
        'extern "C" void* aura_bind_temporary_moving_live_ptr_any_arena(void* p) noexcept;',
        "AC5 bridge decl in test TU",
        test,
    )
    must(
        "bind_temporary_moving_live_ptr_any_arena(raw, &noted)",
        "AC5 adversarial any-arena bind",
        test,
    )
    if test.find("=== Issue #4124: temp canary must bind") < test.find(
        "=== Issue #4066: Moving canary hold through recycle ==="
    ):
        fails.append("AC5: #4124 block must run after the #4066 block")
    for stale in ROOT.glob("docs/design/*4124*"):
        fails.append(f"AC5: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_4124*.cpp"):
        fails.append(f"AC5: forbidden issue test {stale.name}")

    # AC6 — build.py registration + allowlist append.
    must("check_temp_canary_bind_4124.py", "AC6 build.py registration", build)
    must(LINTER, "AC6 allowlist append", allow)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #4124 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    jit = _read(JIT)
    arena = _read(ARENA)
    apply = _read(APPLY)
    test = _read(TEST)
    build = _read(BUILD)
    allow = _read(ALLOWLIST)
    if args.self_test:
        broken = jit.replace("aura_bind_temporary_moving_live_ptr_any_arena", "bind_redacted_4124_")
        self_fails = _rows(broken, arena, apply, test, build, allow)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(jit, arena, apply, test, build, allow)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
