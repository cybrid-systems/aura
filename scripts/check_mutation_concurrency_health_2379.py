#!/usr/bin/env python3
"""Issue #2379: query:mutation-concurrency-health single Agent score.

Contract:
  AC1 Score formula in mutation_concurrency_health.hh
  AC2 force_reason priority steal > residual > densify > hold > mailbox > none
  AC3 Pure / additive register_stats_impl; vacuous health 10000
  AC4 Keys health-bp / force-reason / schema-2379 + components
  AC5 Tests + catalog + gate

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

    hh = _read("src/compiler/mutation_concurrency_health.hh")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    test = _read("tests/compiler/test_mutation_concurrency_health_2379.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 score
    must("health_bp", "AC1", hh)
    must("compute_mutation_concurrency_health", "AC1", hh)
    must("steal_force_deopt_total", "AC1", hh)
    must("densify_consistency_fail_total", "AC1", hh)
    must("mailbox_defer_starvation_total", "AC1", hh)
    must("hold_slo_violation_total", "AC1", hh)
    must("ac1_vacuous_healthy", "AC1", test)

    # AC2 priority
    must("steal-mismatch", "AC2", hh)
    must("residual-defer", "AC2", hh)
    must("densify-fail", "AC2", hh)
    must("hold-slo", "AC2", hh)
    must("mailbox-starvation", "AC2", hh)
    must("ac2_force_reason_and_inject", "AC2", test)

    # AC3 pure
    must("query:mutation-concurrency-health", "AC3", q)
    must("register_stats_impl", "AC3", q)
    must("ac3_pure_identical", "AC3", test)

    # AC4 keys
    must("health-bp", "AC4", q)
    must("force-reason", "AC4", q)
    must("force-reason-code", "AC4", q)
    must("schema-2379", "AC4", q)
    must("mutation-concurrency-health-wired", "AC4", q)
    must("component-steal-force-deopt-total", "AC4", q)
    must("ac4_query_surface", "AC4", test)

    # AC5
    must("Issue #2379", "AC5", q)
    must("query:mutation-concurrency-health", "AC5", obs)
    must("test_mutation_concurrency_health_2379", "AC5", cmake)
    must("check_mutation_concurrency_health_2379", "AC5", build)
    must("cmd_mutation_concurrency_health_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2379 mutation-concurrency-health — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
