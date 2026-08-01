#!/usr/bin/env python3
"""Issue #2505: cross-COW soft-migrate drift threshold vs hard safe-fallback.

  AC1: near-drift soft success
  AC2: far-drift hard + FarBehind reason
  AC3: linear / freed hard only
  AC4: soft disabled → hard + Disabled reason
  AC5: header contract + schema-2505 + gate

Exit 0 = all ACs satisfied.
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

    hh = _read("src/compiler/aura_jit_bridge.h")
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    test = _read("tests/compiler/test_cross_cow_drift_contract_2505.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "AC1", hh)
    must("cross_cow_soft_migrate_max_drift_", "AC1", rt)
    must("ac1_near_drift_soft", "AC1", test)

    must("FarBehind", "AC2", rt)
    must("cross_cow_hard_reject_far_behind_total", "AC2", obs)
    must("ac2_far_drift_hard", "AC2", test)

    must("Linear", "AC2", rt)  # enum used for AC3 path too
    must("cross_cow_hard_reject_linear_total", "AC3", obs)
    must("ac3_linear_and_freed", "AC3", test)

    must("Disabled", "AC4", rt)
    must("cross_cow_hard_reject_disabled_total", "AC4", obs)
    must("ac4_soft_disabled", "AC4", test)

    must("single-workspace MVP", "AC5", hh)
    must("call-time", "AC5", hh)
    must("aura_bump_cross_cow_hard_reject_reason", "AC5", br)
    must("schema-2505", "AC5", q)
    must("cross-cow-call-time-only-wired", "AC5", q)
    must("test_cross_cow_drift_contract_2505", "AC5", cmake)
    must("check_cross_cow_drift_contract_2505", "AC5", build)
    must("cmd_cross_cow_drift_contract_coverage", "AC5", build)
    must("ac5_docs_query_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2505 cross-COW drift contract — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
