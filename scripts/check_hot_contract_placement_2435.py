#!/usr/bin/env python3
"""Issue #2435: hot vs cold contract placement (production hot OFF).

Contract:
  AC1 production default hot OFF under NDEBUG
  AC2 cold edges keep language pre / pass entry
  AC3 absolute-hot loops use AURA_HOT_* (elided under OFF)
  AC4 cold/debug catch policy + AURA_COLD_CONTRACT
  AC5 schema-2435 + test/gate

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

    hh = _read("src/core/cpp26_contract_stats.h")
    soa = _read("src/compiler/ir_soa.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_hot_contract_placement_2435.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2435", "AC1", hh)
    must("AURA_HOT_MODE_OFF", "AC1", hh)
    must("production", "AC1", hh.lower())
    must("kHotContractsMode", "AC1", hh)
    must("2435 AC1", "AC1", test)

    # AC2
    must("pre(func_idx < functions.size())", "AC2", soa)
    must("AURA_COLD_CONTRACT", "AC2", hh)
    must("2435 AC2", "AC2", test)

    # AC3
    must("AURA_HOT_CHECK_CONSTEXPR", "AC3", hh)
    must("AURA_HOT_CHECK_CONSTEXPR", "AC3", soa)
    must("AURA_HOT_RECORD()", "AC3", hh)
    must("2435 AC3", "AC3", test)

    # AC4
    must("AURA_COLD_CONTRACT", "AC4", hh)
    must("2435 AC4", "AC4", test)

    # AC5
    must("schema-2435", "AC5", q)
    must("hot-contracts-mode", "AC5", q)
    must("hotpath_contracts_2435_active", "AC5", hh)
    must("2435 AC5", "AC5", test)
    must("check_hot_contract_placement_2435", "gate", build)
    must("cmd_hot_contract_placement_coverage", "gate", build)
    must("test_hot_contract_placement_2435", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: hot contract placement #2435 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
