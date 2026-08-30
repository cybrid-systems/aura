#!/usr/bin/env python3
"""Issue #3443: FFI/JIT live ptrs outside opaque_heap_ fail-closed.

EXEMPT is observe-only. Arena-tracked + EXEMPT + no slot + no canary
under production required + Moving must fail-closed on the existing
untracked / incomplete / breach face. opaque_heap_ remains FFI cover
SSOT. JIT/module cached create<T>* Env* promote to lasting slots.
No second pin registry. Soft: one atomic / empty-inventory check.

Contract:
  AC1 required+Moving uncovered FFI/JIT ptr → pin_contract_held=false
  AC2 opaque_heap_ slots remain FFI cover SSOT (still remap green)
  AC3 EXEMPT taxonomy unchanged (libc-heap / external-native-addr)
  AC4 JIT/AOT must not cache create<T> raw addrs; modules_ slot walk
  AC5 Soft/Off/no Moving: zero extra pin / walk
  AC6 extend fail_closed + root-closure; no test_issue_3443.cpp

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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    arena = _read("src/core/arena.ixx")
    lp = _read("src/core/lifetime_pin.hh")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ffi = _read("src/compiler/ffi_primitives_impl.cpp")
    dcr = _read("src/core/densify_consistency_report.h")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    loop = _read("tests/compiler/test_densify_root_closure_closed_loop.cpp")
    build = _read("build.py")

    must("Issue #3443" in arena, "AC1: arena.ixx cites #3443")
    must("note_ffi_opaque_create_exempt(void* p, const char* reason)" in arena, "AC1: pointer-form helper")
    must("note_temporary_moving_live_ptr(p)" in arena, "AC1: required+Moving notes #3210 canary")
    must("3443 AC1" in test, "AC1: fail-closed test")
    must("ac3443_1_uncovered_ffi_required_fail_closed" in test, "AC1: AC6 uncovered FFI")

    must("for (void*& op : opaque_heap_)" in mb, "AC2: opaque_heap_ SSOT walk kept")
    must("3443 AC2" in test, "AC2: opaque_heap_ green test")
    must("ac3443_2_opaque_heap_slot_still_green" in test, "AC2: slot remap test")

    must("GENERAL_OBJECT_PIN_EXEMPT: external-native-addr" in ffi, "AC3: native EXEMPT")
    must("GENERAL_OBJECT_PIN_EXEMPT: libc-heap" in ffi, "AC3: libc EXEMPT")
    must("libc-heap |" in lp and "external-native-addr" in lp, "AC3: taxonomy")
    must("3443 AC3" in test, "AC3: taxonomy test")

    must("for (auto*& m : modules_)" in mb, "AC4: modules_ slot walk")
    must("require_inject_env_" in mb, "AC4: require_inject_env_ slot")
    must("lasting void** slot in modules_" in flat, "AC4: inst-env slot not EXEMPT")
    must('"inst-env-cache-transient"' not in flat, "AC4: moving Env* EXEMPT removed")
    must("3443 AC4" in test, "AC4: modules walk test")
    must("3443 AC4" in loop, "AC4: root-closure extended")

    must("general_object_pin_required_active()" in arena, "AC5: required gate")
    must("moving_compact_enabled()" in arena, "AC5: Moving gate")
    must("3443 AC5" in test, "AC5: Soft test")
    must("g_3443_" not in arena and "g_3443_" not in mb, "AC5: no g_3443_*")
    must("schema-3443" not in arena and "schema-3443" not in dcr, "AC6: no schema-3443")

    must("kFfiJitLivePtrInventoryIssue = 3443" in dcr, "AC6: stamp")
    must("check_ffi_jit_live_ptr_inventory_3443" in build, "AC6: build.py")
    must("3443 AC6" in test, "AC6: no-invent test")
    must(not (ROOT / "tests/core/test_issue_3443.cpp").is_file(), "AC6: no invent core")
    must(not (ROOT / "tests/issues/test_issue_3443.cpp").is_file(), "AC6: no invent issues")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3443-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if "class FfiJitPinRegistry" in mb or "g_ffi_jit_registry_3443" in arena:
        fails.append("AC4: second pin registry introduced")

    if fails:
        print("FAIL #3443 ffi_jit_live_ptr_inventory:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3443 ffi_jit_live_ptr_inventory: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
