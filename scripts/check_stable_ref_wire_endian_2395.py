#!/usr/bin/env python3
"""Issue #2395: StableNodeRef wire multi-byte fields are little-endian.

Contract:
  AC1 write_u*_le / read_u*_le used in serialize/deserialize
  AC2 Layout docs state little-endian for multi-byte fields
  AC3 No host-endian memcpy of multi-byte StableNodeRef fields
  AC4 Golden LE + swap-corruption tests
  AC5 CMake + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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
            fails.append(f"{label}: unexpected {n!r}")

    stab = _read("src/core/ast_stability.cpp")
    ixx = _read("src/core/ast.ixx")
    test = _read("tests/core/test_stable_ref_wire_endian_2395.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 helpers
    must("Issue #2395", "AC1", stab)
    must("write_u16_le", "AC1", stab)
    must("write_u32_le", "AC1", stab)
    must("write_u64_le", "AC1", stab)
    must("read_u16_le", "AC1", stab)
    must("read_u32_le", "AC1", stab)
    must("read_u64_le", "AC1", stab)
    must("write_u32_le(out + 4, ref.id)", "AC1", stab)
    must("read_u32_le(buf.data() + 4)", "AC1", stab)

    # AC2 docs
    must("little-endian", "AC2", ixx)
    must("2395", "AC2", ixx)

    # AC3 no host memcpy of multi-byte fields in serialize
    must_not("std::memcpy(out + 4, &ref.id", "AC3", stab)
    must_not("std::memcpy(out + 8, &ref.gen", "AC3", stab)
    must_not("std::memcpy(&r.id, buf.data() + 4", "AC3", stab)

    # AC4 tests
    must("ac1_roundtrip", "AC4", test)
    must("ac2_golden_le_bytes", "AC4", test)
    must("ac3_hand_built_v1_le", "AC4", test)
    must("ac4_forced_swap_corrupts", "AC4", test)
    must("ac5_source_and_gate", "AC5", test)

    # AC5 registration
    must("test_stable_ref_wire_endian_2395", "AC5", cmake)
    must("check_stable_ref_wire_endian_2395", "AC5", build)
    must("cmd_stable_ref_wire_endian_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2395 StableNodeRef wire little-endian — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
