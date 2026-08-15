#!/usr/bin/env python3
"""Issue #3057: FFI opaque / native buffer true pin-or-remap / slot / EXEMPT.

Closes #3022 residual (create-point observe is not remap cover).

Contract (one row per AC):
  AC1  Production required + Moving: densify-tracked FFI opaque_heap_
       aliases are slotted; uncovered alias fail-closes (stale canary /
       pin_contract_held=false)
  AC2  Soft / required-off / Moving disabled: known-root walk not taken
  AC3  EXEMPT still carries reason; no silent bypass
  AC4  No second registry; reuse LifetimePin + external-root-slot
  AC5  Extend fail_closed + pin + coverage_gate; no test_issue_3057.cpp;
       no docs/design/ (#1655)

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
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    ffi = _read("src/compiler/ffi_primitives_impl.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    pin = _read("tests/core/test_general_object_pin.cpp")
    gate = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    build = _read("build.py")

    must("kFfiOpaquePinOrRemapResidualIssue = 3057", "AC1 stamp", lp)
    must("for (void*& op : opaque_heap_)", "AC1 walk", mb)
    must("ac3057_1_ffi_alias_slot_remaps", "AC1 remap test", test)
    must("ac3057_2_uncovered_ffi_alias_fail_closed", "AC1 fail-closed test", test)

    must("ac3057_3_soft_no_walk", "AC2 test", test)
    must("moving_compact_enabled()", "AC2 Moving gate", mb)

    must("GENERAL_OBJECT_PIN_EXEMPT: external-native-addr", "AC3 c-opaque", ffi)
    must("GENERAL_OBJECT_PIN_EXEMPT: libc-heap", "AC3 c-alloc", ffi)
    must("GENERAL_OBJECT_PIN_EXEMPT: opaque-struct-copy", "AC3 struct-ref", ffi)
    must("ac3057_4_exempt_reason_no_second_registry", "AC3 test", test)

    must("no second registry", "AC4", mb)
    must("register_external_root_slot_for_densify_all", "AC4 slot", mb)
    must("opaque_heap_", "AC4 evaluator cite", ixx)

    must("ac3057_5_source_cite_no_invent", "AC5 test", test)
    must("ac3057_ffi_opaque_slot_cover", "AC5 pin suite", pin)
    must("ac3057_coverage_gate_cite", "AC5 gate", gate)
    must("schema-3057", "AC5 schema", obs)
    must("check_ffi_opaque_pin_or_remap_3057", "AC5 build", build)
    if _read("docs/design/3057-ffi-opaque-slot.md"):
        fails.append("AC5: docs/design/3057-* present")
    if _read("tests/core/test_issue_3057.cpp"):
        fails.append("AC5: test_issue_3057.cpp present")
    if "class FfiOpaquePinRegistry" in mb or "g_ffi_opaque_registry_3057" in mb:
        fails.append("AC5: second pin registry introduced")

    if fails:
        print(f"Issue #3057 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3057 FFI opaque pin-or-remap residual — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
