#!/usr/bin/env python3
"""Issue #2300: query:lifetime-contract-snapshot coverage linter.

  AC1: pure idle ok formula + query keys
  AC2: MutationHold + linear live counts path
  AC3: pin-miss / linear-miss / residual force_reason
  AC4: additive schema-2300; existing gc-defer query retained
  AC5: source-cite make_lifetime_contract_snapshot + tests

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
H = ROOT / "src" / "core" / "lifetime_contract.h"
Q = ROOT / "src" / "compiler" / "evaluator_primitives_obs_jit.cpp"
TEST = ROOT / "tests" / "compiler" / "test_lifetime_contract_snapshot_2300.cpp"
CMAKE = ROOT / "CMakeLists.txt"


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    h = H.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")
    cmake = CMAKE.read_text(encoding="utf-8", errors="replace")

    must("make_lifetime_contract_snapshot", "AC1", h)
    must("kLifetimeContractIssue = 2300", "AC1", h)
    must("query:lifetime-contract-snapshot", "AC1", q)
    must("lifetime-contract-ok", "AC1", q)
    must("AC1: pure idle → ok", "AC1", test)

    must("MutationHold", "AC2", test)
    must("pin_linear_root", "AC2", test)
    must("AC2: pure linear count stable", "AC2", test)

    must("pin-miss", "AC3", h)
    must("linear-miss", "AC3", h)
    must("AC3: force_reason=pin-miss", "AC3", test)
    must("AC3: residual hard fail → ok=0", "AC3", test)

    must("schema-2300", "AC4", q)
    must("issue-2300", "AC4", q)
    must("lifetime-contract-wired", "AC4", q)
    must("query:gc-defer-reason-stats", "AC4", q)
    must("test_lifetime_contract_snapshot_2300", "AC4", cmake)

    must("Formula", "AC5", h)
    must("ac5_source_cite", "AC5", test)
    must("no mutate side effects", "AC5", test.lower())

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: lifetime-contract-snapshot (#2300) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
