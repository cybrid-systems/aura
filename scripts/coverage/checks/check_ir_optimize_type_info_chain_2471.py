#!/usr/bin/env python3
"""Issue #2471: optimize_type_info chain-walk terminates on MAX sentinel.

Contract:
  AC1 multi-step chain through slot 0
  AC2 termination uses MAX not remap==0
  AC3 simple elim retained
  AC4 source cite
  AC5 gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    ir = _read("src/compiler/ir.ixx")
    test = _read("tests/compiler/test_ir_optimize_type_info_chain.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate optimize_type_info chain walk body
    idx = ir.find("void optimize_type_info()")
    body = ir[idx : idx + 3500] if idx >= 0 else ""

    must("Issue #2471", "AC1", ir)
    must("optimize_type_info", "AC1", ir)
    must("2471 AC1", "AC1", test)
    must("slot 5", "AC1", test.lower() + test)  # multi-step to 5

    must("numeric_limits<std::uint32_t>::max()", "AC2", body)
    must_not("slot_remap[src] != 0", "AC2", body)
    must("Do NOT terminate on remap==0", "AC2", body)

    must("2471 AC3", "AC3", test)
    must("CastOp", "AC3", body)

    must("Issue #2471", "AC4", ir)
    must("2471 AC4", "AC4", test)

    must("check_ir_optimize_type_info_chain_2471", "gate", build)
    must("cmd_ir_optimize_type_info_chain_coverage", "gate", build)
    must("test_ir_optimize_type_info_chain", "gate", cmake)
    must("2471 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: IR optimize_type_info chain walk #2471 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
