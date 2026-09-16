#!/usr/bin/env python3
"""Issue #3834: IR Call primitive arm ownerless production fail-closed (#3798 sibling).

PrimCall (#3798) / HashRemove (#3720) skip raw (*pfn) under
production_defaults_active() + null evaluator. Call's is_primitive branch
must mirror that for mutate-class / FFI / file prims reached via Call.

Contract:
  AC1  Call primitive arm cites production_defaults_active + make_void;
       PrimCall gate retained
  AC2  adversarial ACs live in test_dispatch_required_effects.cpp (#3834)
  AC3  Soft/Off ownerless raw (*pfn)(call_args) retained
  AC4  no new query key; extends #3798/#3720 family

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

    impl = _read("src/compiler/ir_executor_impl.cpp")
    test = _read("tests/compiler/test_dispatch_required_effects.cpp")
    gf = _read("scripts/coverage/simple_check_grandfather.txt")
    build = _read("build.py")

    call = impl.find("case IROpcode::Call")
    if call < 0:
        fails.append("AC1: Call case missing")
    else:
        win = impl[call : call + 7500]
        must("is_primitive(callee_val)", "AC1 Call primitive arm", win)
        must("production_defaults_active()", "AC1 Call production gate", win)
        must("make_void()", "AC1 Call void fail-closed", win)
        must("(*pfn)(call_args)", "AC3 Soft raw pfn(call_args)", win)
        if "#3834" not in win and "Issue #3834" not in win:
            fails.append("AC1: Call missing #3834 cite")
        prim = win.find("is_primitive(callee_val)")
        prod = win.find("production_defaults_active()", prim if prim >= 0 else 0)
        last_raw = win.rfind("(*pfn)(call_args)")
        if prim < 0 or prod < 0 or last_raw < 0 or prod > last_raw:
            fails.append("AC1: production gate not ordered before Soft raw pfn in Call arm")

    pc = impl.find("case IROpcode::PrimCall")
    if pc < 0:
        fails.append("AC1: PrimCall case missing (#3798 family)")
    else:
        must("production_defaults_active()", "AC1 PrimCall gate", impl[pc : pc + 3500])

    must("#3834 AC1", "AC2/AC4 test family", test)
    must("#3834 AC2", "AC2 adversarial", test)
    must("#3834 AC3", "AC3 Soft raw", test)
    must("null evaluator", "AC2 null evaluator", test)
    must("IROpcode::Call", "AC2 Call path", test)
    must("IROpcode::Primitive", "AC2 Primitive load before Call", test)
    must("vector-set!", "AC2 mutate-class Call", test)

    must("check_call_ownerless_fail_closed_3834.py", "AC4 grandfather", gf)
    must("check_call_ownerless_fail_closed_3834", "AC4 build wiring", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: #3834 Call ownerless production fail-closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
