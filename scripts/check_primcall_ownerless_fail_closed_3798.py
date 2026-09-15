#!/usr/bin/env python3
"""Issue #3798: IR PrimCall ownerless production fail-closed (#3720 residual).

HashSet/HashRemove already skip raw (*pfn) under production_defaults_active()
+ null evaluator. PrimCall must mirror that for mutate-class PrimIds
(vector-set!, etc.).

Contract:
  AC1  PrimCall arm cites production_defaults_active + make_void next to
       HashSet/HashRemove pattern
  AC2  adversarial ACs live in test_dispatch_required_effects.cpp (#3798)
  AC3  Soft/Off ownerless raw (*pfn)(pargs) retained
  AC4  no new query key; extends #3720 family (this linter + dispatch test)

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

    impl = _read("src/compiler/ir_executor_impl.cpp")
    test = _read("tests/compiler/test_dispatch_required_effects.cpp")

    pc = impl.find("case IROpcode::PrimCall")
    if pc < 0:
        fails.append("AC1: PrimCall case missing")
    else:
        win = impl[pc : pc + 3200]
        must("production_defaults_active()", "AC1 PrimCall production gate", win)
        must("make_void()", "AC1 PrimCall void fail-closed", win)
        must("(*pfn)(pargs)", "AC3 Soft raw pfn", win)
        if "#3798" not in win and "Issue #3798" not in win:
            fails.append("AC1: PrimCall missing #3798 cite")
        # Fail-closed arm must precede Soft raw (production check before else raw).
        prod = win.find("production_defaults_active()")
        raw = win.find("(*pfn)(pargs)")
        # First (*pfn) may be inside invoke_prim lambda; find Soft branch one.
        # Require production_defaults_active appears before the last (*pfn)(pargs).
        last_raw = win.rfind("(*pfn)(pargs)")
        if prod < 0 or last_raw < 0 or prod > last_raw:
            fails.append("AC1: production gate not ordered before Soft raw pfn")

    hs = impl.find("case IROpcode::HashSet")
    hr = impl.find("case IROpcode::HashRemove")
    if hs < 0 or hr < 0:
        fails.append("AC1: HashSet/HashRemove cases missing (#3720 family)")
    else:
        must("production_defaults_active()", "AC1 HashSet gate", impl[hs : hs + 2000])
        must("production_defaults_active()", "AC1 HashRemove gate", impl[hr : hr + 1200])

    must("#3798 AC1", "AC2/AC4 test family", test)
    must("#3798 AC2", "AC2 adversarial", test)
    must("#3798 AC3", "AC3 Soft raw", test)
    must("null evaluator", "AC2 null evaluator", test)
    must("production_defaults_active", "AC2 production arm", test)
    if "PrimId::VectorSet" not in test:
        fails.append("AC2: mutate-class PrimCall (PrimId::VectorSet) missing in tests")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: #3798 PrimCall ownerless production fail-closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
