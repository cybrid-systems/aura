#!/usr/bin/env python3
"""Issue #2394: last_validated_generation concurrent-safe (atomic).

Contract:
  AC1 Concurrent validate_with_provenance uses atomic store
  AC2 CopyableAtomicU16 preserves StableNodeRef copy/aggregate semantics
  AC3 Wire serialize/deserialize load/store (no memcpy of atomic)
  AC4 Source-cite + relaxed memory_order
  AC5 Tests + CMake + build.py gate

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

    ixx = _read("src/core/ast.ixx")
    stab = _read("src/core/ast_stability.cpp")
    test = _read("tests/core/test_last_validated_generation_atomic_2394.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1–AC2 type
    must("Issue #2394", "AC1", ixx)
    must("CopyableAtomicU16", "AC1", ixx)
    must("std::atomic<std::uint16_t>", "AC1", ixx)
    must("last_validated_generation", "AC1", ixx)
    must("CopyableAtomicU16 last_validated_generation", "AC2", ixx)

    # AC1 store path
    must("Issue #2394", "AC1", stab)
    must("memory_order_relaxed", "AC1", stab)
    must("last_validated_generation.store", "AC1", stab)
    must("ac1_ac2_concurrent_validate", "AC1", test)

    # AC3 wire path uses load/store not memcpy of atomic object
    must("load(std::memory_order_relaxed)", "AC3", stab)
    if "memcpy(out + 18, &ref.last_validated_generation" in stab:
        fails.append("AC3: still memcpy of atomic last_validated_generation on serialize")
    if "memcpy(&r.last_validated_generation" in stab:
        fails.append("AC3: still memcpy into atomic last_validated_generation on deserialize")

    # AC4–AC5
    must("ac3_copy_and_assign", "AC4", test)
    must("ac4_ac5_source_and_gate", "AC5", test)
    must("test_last_validated_generation_atomic_2394", "AC5", cmake)
    must("check_last_validated_generation_atomic_2394", "AC5", build)
    must("cmd_last_validated_generation_atomic_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2394 last_validated_generation atomic — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
