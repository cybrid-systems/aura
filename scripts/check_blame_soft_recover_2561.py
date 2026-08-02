#!/usr/bin/env python3
"""Issue #2561: Soft/Sampled blame chain recover + miss escalate coverage.

Contract:
  AC1 try_recover_blame_chain_soft + maybe_soft_recover_or_escalate_blame
  AC2 complete / no-miss zero-work path (had_miss_signal gate)
  AC3 Soft observe default; Full/#2221 hard path preserved
  AC4 schema-2561 on fidelity + health queries; source-cite
  AC5 recovery_mode does not redefine miss / blame_commit counters;
      test + cmake + build.py gate

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

    pol = _read("src/compiler/coercion_provenance_policy.hh")
    cmap = _read("src/compiler/coercion_map.ixx")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_blame_soft_recover_2561.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2561", "AC1", pol)
    must("g_blame_soft_recover_total", "AC1", pol)
    must("g_blame_soft_recover_fail_total", "AC1", pol)
    must("g_blame_soft_escalate_total", "AC1", pol)
    must("try_recover_blame_chain_soft", "AC1", cmap)
    must("maybe_soft_recover_or_escalate_blame", "AC1", cmap)
    must("ac1_recover_or_escalate", "AC1", test)

    # AC2
    must("had_miss_signal", "AC2", cmap)
    must("ac2_complete_zero_work", "AC2", test)

    # AC3
    must("AURA_BLAME_SOFT_ESCALATE", "AC3", pol)
    must("observe-by-default", "AC3", pol)
    must("#2221", "AC3", pol)
    must("AuditStrategy::Full", "AC3", cmap)
    must("ac3_full_and_soft_observe", "AC3", test)

    # AC4
    must("schema-2561", "AC4", q)
    must("blame-soft-recover-total", "AC4", q)
    must("query:type-incremental-fidelity-stats", "AC4", q)
    must("maybe_soft_recover_or_escalate_blame", "AC4", bound)
    must("#2561", "AC4", bound)
    must("ac4_schema_source", "AC4", test)

    # AC5
    must("recovery_mode", "AC5", cmap)
    must("if (!recovery_mode)", "AC5", cmap)
    must("test_blame_soft_recover_2561", "AC5", cmake)
    must("check_blame_soft_recover_2561", "AC5", build)
    must("cmd_blame_soft_recover_coverage", "AC5", build)
    must("ac5_existing_authoritative", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2561 Soft blame recover/escalate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
