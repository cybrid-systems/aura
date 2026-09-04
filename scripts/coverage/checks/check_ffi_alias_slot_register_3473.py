#!/usr/bin/env python3
"""Issue #3473: slotted FFI cover must register, not metric-only.

#3443/#3274 closed the triad. note_ffi_opaque_alias_densify_cover still
bumped g_ffi_opaque_alias_slot_cover_total and returned without
register_external_root_slot_for_densify, so a non-inventory void** was
not rewritten after objects_moved>0.

Contract:
  AC1 slot != null → register (process queue drained at live_compact)
  AC2 slot path does not canary (#3368 XOR)
  AC3 no-slot path still #3210 canary
  AC4 Soft / !Moving: exempt bump, no register, no canary
  AC5 extend fail_closed; no test_issue_3473.cpp / no new query key

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    arena = _read("src/core/arena.ixx")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")

    fn = arena.find("export inline void note_ffi_opaque_alias_densify_cover")
    nxt = arena.find("export inline void note_ffi_opaque_create_exempt(void* p", fn + 1) if fn >= 0 else -1
    win = arena[fn:nxt] if fn >= 0 and nxt > fn else arena[fn : fn + 1800] if fn >= 0 else ""
    must("Issue #3473", "AC1 cite", win)
    must("register_external_root_slot_for_densify(slot)", "AC1 register", win)
    slot = win.find("if (slot != nullptr && *slot != nullptr)")
    canary = win.find("note_temporary_moving_live_ptr(p)")
    if slot < 0 or canary < 0 or not (slot < canary):
        fails.append("AC1: slot branch must precede no-slot canary")
    else:
        branch = win[slot:canary]
        if "register_external_root_slot_for_densify(slot)" not in branch:
            fails.append("AC1: slot branch does not register")
        if "note_temporary_moving_live_ptr" in branch:
            fails.append("AC2: slot branch dual-notes canary (#3368)")
        if "g_ffi_opaque_alias_slot_cover_total" not in branch:
            fails.append("AC1: keep existing slot-cover metric")
    must("note_temporary_moving_live_ptr(p)", "AC3 no-slot canary", win)
    must("moving_compact_enabled()", "AC4 Soft gate", win)
    must("note_ffi_opaque_create_exempt(reason)", "AC4 exempt fallback", win)

    drain = arena.find("if (snapshot_ffi_alias_slots_for_densify(alias_slots)")
    reloc = arena.find("result.objects_moved = relocate_tracked_objects_for_moving_")
    if drain < 0 or reloc < 0 or not (drain < reloc):
        fails.append("AC1: process slots must drain before relocate")

    must("3473 AC1: *slot rewritten to remapped address", "AC5 remap test", test)
    must("3473 AC2: slotted path does not canary (#3368 XOR)", "AC5 XOR test", test)
    must("3473 AC4: no slot-cover bump", "AC5 Soft test", test)
    must_not("schema-3473", "AC5 no query key", arena)
    must_not("g_3473_", "AC5 no g_3473_*", arena)
    must_not("class FfiOpaquePinRegistry", "AC5 no second registry", arena)

    if (ROOT / "tests" / "core" / "test_issue_3473.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3473.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3473.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3473.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3473-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3473 ffi_alias_slot_register:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3473 ffi_alias_slot_register: slotted cover registers; XOR canary kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
