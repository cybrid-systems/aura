#!/usr/bin/env python3
"""Issue #2608: optional OccurrenceGoal persist / rehydrate.

Contract:
  AC1 append_occurrence_snapshot + rehydrate_occurrence_from_persist
  AC2 soft default OFF (occurrence_persist_enabled / AURA_OCCURRENCE_PERSIST)
  AC3 cap + occurrence_persist_trunc_total
  AC4 schema-2608 + metrics fields + query keys
  AC5 test + cmake + build.py gate; no docs/design
  AC6 #2641: production-default persist + rehydrate-miss observability (extends AC5)

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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_occurrence_goal_persist_rehydrate_2608.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2608", "AC1", ixx)
    must("OccurrencePersistEntry", "AC1", ixx)
    must("append_occurrence_snapshot", "AC1", ixx + impl)
    must("rehydrate_occurrence_from_persist", "AC1", ixx + impl)
    must("maybe_persist_occurrence_snapshot", "AC1", ixx + mb)

    # AC2 soft
    must("occurrence_persist_enabled", "AC2", ixx + impl)
    must("AURA_OCCURRENCE_PERSIST", "AC2", impl)
    must("production_defaults_active", "AC2", impl)

    # AC3 cap
    must("occurrence_persist_cap", "AC3", ixx + impl)
    must("occurrence_persist_trunc_total", "AC3", met + fields + impl)
    must("AURA_OCCURRENCE_PERSIST_CAP", "AC3", impl)

    # AC4 metrics/query
    must("occurrence_persist_write_total", "AC4", met + fields + impl)
    must("occurrence_rehydrate_total", "AC4", met + fields + impl)
    must("schema-2608", "AC4", q)
    must("occurrence-persist-wired", "AC4", q)
    must("occurrence-rehydrate-total", "AC4", q)

    # fence + solve hooks
    must("rehydrate_occurrence_from_persist", "AC1-fence", ixx)
    must("rehydrate_occurrence_from_persist", "AC1-sdo", impl)

    # AC5
    must("ac1_persist_prune_rehydrate", "AC5", test)
    must("ac2_soft_zero_writes", "AC5", test)
    must("test_occurrence_goal_persist_rehydrate_2608", "AC5", cmake)
    must("check_occurrence_goal_persist_rehydrate_2608", "AC5", build)
    must("cmd_occurrence_goal_persist_rehydrate_coverage", "AC5", build)

    # Issue #2641 AC6: schema + source-cite + coverage gate.
    # Extends AC5 with the production-default persist ON path + miss
    # observability (occurrence_persist_rehydrate_miss_total counter).
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met2641 = _read("src/compiler/observability_metrics.h")
    fields2641 = _read("src/compiler/compiler_metrics_fields.inc")
    q2641 = _read("src/compiler/evaluator_primitives_query.cpp")
    # #2641 source-cite
    must("#2641", "AC6", ixx)
    must("#2641", "AC6", mb)
    # Miss counter (the key new observability for #2641 AC4)
    must("occurrence_persist_rehydrate_miss_total", "AC6", met2641)
    must("occurrence_persist_rehydrate_miss_total", "AC6", fields2641)
    # Dtor-side helper (the production-default ON wire-up)
    must("aura_outermost_success_persist_occurrence", "AC6", mb)
    # Query surface (additive — preserves #2608 keys)
    must("schema-2641", "AC6", q2641)
    must("issue-2641", "AC6", q2641)
    must("occurrence-persist-prod-default-wired", "AC6", q2641)
    must("occurrence-persist-rehydrate-miss-total", "AC6", q2641)
    # Test functions for #2641 ACs
    must("ac2641_1_production_default_persist", "AC6", test)
    must("ac2641_3_env_zero_forces_off", "AC6", test)
    must("ac2641_4_rehydrate_miss_counter", "AC6", test)
    must("ac2641_6_source_cite", "AC6", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2608 + #2641 OccurrenceGoal persist/rehydrate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
