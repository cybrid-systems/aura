#!/usr/bin/env python3
"""Issue #3274: densify-tracked FFI opaque aliases — create-point observe is
not pin/slot/remap cover.

#3022/#3057 introduced note_ffi_opaque_create_exempt (observe-only counter).
Residual: a densify-tracked FFI opaque / native buffer alias that is only
observed at create can remain as a last_object_remap_ key after Moving
densify with objects_moved>0 (stale pointer / UAF window) when the Moving
window skips the Evaluator known-root walk (arena auto-arm path). #3274
closes the triad for this allocate class:

  AC1  no stable void** slot → #3210 temp canary inventory cover
       (fail-closed: incomplete_remap / sticky / pin_contract_held=false)
  AC2  stable void** slot → slot-rewrite cover (no stale, remap clean);
       slot and canary are EXCLUSIVE for the same pointer
  AC3  Soft / Off / unset → zero extra (falls back to EXEMPT)
  AC4  additive slot-cover counter only; no second registry; no new query:*
  AC5  FFI create sites wired (opaque-struct-copy, ffi-return-external);
       libc-heap / external-native-addr keep EXEMPT (true non-arena);
       no test_issue_3274.cpp (#81967); no docs/design/ (#1655);
       build.py wires linter

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
    dc = _read("src/core/densify_consistency_report.h")
    ffi = _read("src/compiler/ffi_primitives_impl.cpp")
    ev = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    must("note_ffi_opaque_alias_densify_cover", "AC1/AC2 helper", arena)
    must("kFfiOpaqueDensifyAliasCoverIssue = 3274", "AC4 stamp", dc)
    must("g_ffi_opaque_alias_slot_cover_total", "AC4 counter", dc)
    must("ffi_opaque_alias_slot_cover_total_v_read", "AC4 accessor", dc)
    must("note_temporary_moving_live_ptr(p)", "AC1 canary cover", arena)
    must("Slot and canary are EXCLUSIVE", "AC2 exclusivity doc", arena)
    must("3274 AC1", "AC1 test", test)
    must("3274 AC2", "AC2 test", test)
    must("3274 AC3", "AC3 test", test)
    must("3274 AC4", "AC4 test", test)
    must("3274 AC5", "AC5 test", test)
    must("note_ffi_opaque_alias_densify_cover", "AC5 ffi wire", ffi)
    must("note_ffi_opaque_alias_densify_cover", "AC5 eval wire", ev)
    must('note_ffi_opaque_create_exempt("libc-heap")', "AC5 libc exempt", ffi)
    must('note_ffi_opaque_create_exempt("external-native-addr")', "AC5 native exempt", ffi)
    must("check_ffi_opaque_densify_cover_3274", "AC5 build.py", build)
    if "query:ffi-opaque-cover" in arena or "query:opaque-densify" in arena:
        fails.append("AC4: new query:* (reuse existing surfaces)")
    if "class FfiOpaquePinRegistry" in arena or "g_ffi_opaque_registry_3274" in arena:
        fails.append("AC4: second pin registry introduced")
    if _read("tests/core/test_issue_3274.cpp"):
        fails.append("AC5: test_issue_3274.cpp present (forbidden #81967)")
    if _read("docs/design/3274-ffi-opaque-densify-cover.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3274 ffi_opaque_densify_cover:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3274 ffi_opaque_densify_cover: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
