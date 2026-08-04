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

ROOT = Path(__file__).resolve().parents[3]
H = ROOT / "src" / "core" / "lifetime_contract.h"
DCR = ROOT / "src" / "core" / "densify_consistency_report.h"  # Issue #2341
RMP = ROOT / "src" / "compiler" / "root_remap_pass.ixx"  # Issue #2341
EMB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"  # Issue #2341
Q = ROOT / "src" / "compiler" / "evaluator_primitives_obs_jit.cpp"
TEST = ROOT / "tests" / "compiler" / "test_lifetime_contract_snapshot_2300.cpp"
CMAKE = ROOT / "CMakeLists.txt"


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    h = H.read_text(encoding="utf-8", errors="replace")
    dcr = DCR.read_text(encoding="utf-8", errors="replace")
    rmp = RMP.read_text(encoding="utf-8", errors="replace")
    emb = EMB.read_text(encoding="utf-8", errors="replace")
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

    # AC_2341: Issue #2341 — unified post-densify consistency probe.
    # Refines #2266 · #2280 · #2294 · #2297 · #2295 · #2300; production
    # review (2026-07-29) 建议 5. Closes the residual half-success blind
    # spot under densify × steal where pin ok / remount fail is hard to
    # see without joining multiple schemas.
    must("DensifyConsistencyReport", "AC_2341", dcr)
    must("force_reason", "AC_2341", dcr)
    must("overall_ok", "AC_2341", dcr)
    must("g_densify_consistency_fail_total", "AC_2341", dcr)
    must("densify_consistency_fail_total", "AC_2341", dcr)
    must("bump_densify_consistency_fail_total", "AC_2341", dcr)
    must("densify_consistency_hard_contract_enabled", "AC_2341", dcr)
    must("Issue #2341", "AC_2341", dcr)
    # root_remap_pass.ixx: last-result semantic + getter for Phase 5.
    must("g_last_root_remap_any_fail", "AC_2341", rmp)
    must("last_root_remap_any_fail", "AC_2341", rmp)
    must("Issue #2341", "AC_2341", rmp)
    # evaluator_mutation_boundary.cpp: Phase 5 driver wire-up.
    must("DensifyConsistencyReport", "AC_2341", emb)
    must("bump_densify_consistency_fail_total", "AC_2341", emb)
    must("AURA_DENSIFY_CONTRACT", "AC_2341", emb)
    must("Issue #2341", "AC_2341", emb)
    # evaluator_primitives_obs_jit.cpp: query surface additive keys.
    must("import aura.compiler.root_remap_pass", "AC_2341", q)
    must("densify-consistency-ok", "AC_2341", q)
    must("densify-force-reason-code", "AC_2341", q)
    must("densify-consistency-fail-total", "AC_2341", q)
    must("densify-consistency-wired", "AC_2341", q)
    must("schema-2341", "AC_2341", q)
    must("issue-2341", "AC_2341", q)
    # test_lifetime_contract_snapshot_2300.cpp: ac2341_* test functions.
    must("void ac2341_1_report_default_ok", "AC_2341", test)
    must("void ac2341_2_force_reason_priority", "AC_2341", test)
    must("void ac2341_3_counter_queryable", "AC_2341", test)
    must("void ac2341_4_query_schema", "AC_2341", test)
    must("void ac2341_5_source_cite", "AC_2341", test)
    must("Issue #2341", "AC_2341", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: lifetime-contract-snapshot (#2300) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
