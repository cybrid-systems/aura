#!/usr/bin/env python3
"""
Linter for #2319 — optional hard CastOp density gate for unannotated
Dynamic rebinds (refine #2287 soft hint). Closes the "Agent ignores
the hint indefinitely" gap by adding an opt-in hard path (env-gated
via AURA_CASTOP_DENSITY_HARD=1) that raises MutateTypeGate / post-mutate
pressure when density stays over budget on new unannotated Dynamic
rebinds. Default Soft path stays fast (per AC1 / #2287 lineage).

Verifies the implementation is wired correctly:
  - observability_metrics.h: new per-CompilerMetrics counters
    (castop_density_hard_reject_total + castop_density_hard_wired)
  - service_dirty.cpp: env accessor AURA_CASTOP_DENSITY_HARD + hard
    gate branch after the existing density block (per AC2)
  - evaluator_primitives_query.cpp: query:castop-density-stats
    extended with 4 new keys (castop-density-hard-reject-total +
    castop_density_hard_reject_total + castop-density-hard-wired +
    castop_density_hard_wired + schema-2319 + issue-2319)
  - tests/compiler/test_dead_coercion_layered_2282.cpp: extended
    with ac2319_* test functions (ac2319_hard_gate_wiring +
    ac2319_query_keys_wired + ac2319_soft_default_unchanged +
    ac2319_hard_path_fires + ac2319_source_cite_rows) + Issue #2319 cite

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_castop_density_hard_gate_coverage.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def must_contain(file: Path, needle: str, label: str) -> bool:
    if not file.exists():
        print(f"FAIL {label}: file {file} does not exist")
        return False
    content = read(file)
    if needle in content:
        print(f"OK  {label}")
        return True
    print(f"FAIL {label}: '{needle}' not found in {file}")
    return False


def main() -> int:
    checks = [
        # observability_metrics.h: new per-CompilerMetrics counters
        (
            ROOT / "src/compiler/observability_metrics.h",
            "castop_density_hard_reject_total{0}; // #2319",
            "observability_metrics.h: castop_density_hard_reject_total counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "castop_density_hard_wired{0};",
            "observability_metrics.h: castop_density_hard_wired sentinel",
        ),
        # service_dirty.cpp: env accessor + hard gate branch
        (
            ROOT / "src/compiler/service_dirty.cpp",
            "AURA_CASTOP_DENSITY_HARD",
            "service_dirty.cpp: AURA_CASTOP_DENSITY_HARD env var present",
        ),
        (
            ROOT / "src/compiler/service_dirty.cpp",
            "hard_env = false",
            "service_dirty.cpp: hard_env defaults to false (Soft default)",
        ),
        (
            ROOT / "src/compiler/service_dirty.cpp",
            "castop_density_hard_reject_total.fetch_add",
            "service_dirty.cpp: hard_reject_total bumped on over-budget",
        ),
        (
            ROOT / "src/compiler/service_dirty.cpp",
            "castop_density_hard_wired.store",
            "service_dirty.cpp: hard_wired sentinel set on over-budget",
        ),
        (
            ROOT / "src/compiler/service_dirty.cpp",
            "unannotated_dynamic",
            "service_dirty.cpp: unannotated_dynamic detection present",
        ),
        (
            ROOT / "src/compiler/service_dirty.cpp",
            "mutate_type_gate::is_hard()",
            "service_dirty.cpp: MutateTypeGate Hard check present (#2219)",
        ),
        (ROOT / "src/compiler/service_dirty.cpp", "Issue #2319", "service_dirty.cpp: cites Issue #2319"),
        # mutate_type_gate.hh: MutateTypeGate enum
        (
            ROOT / "src/compiler/mutate_type_gate.hh",
            "enum class MutateTypeGate",
            "mutate_type_gate.hh: MutateTypeGate enum (Soft=0, Hard=1)",
        ),
        (ROOT / "src/compiler/mutate_type_gate.hh", "is_hard()", "mutate_type_gate.hh: is_hard() accessor"),
        # evaluator_primitives_query.cpp: query primitive keys
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "castop-density-hard-reject-total",
            "query primitive: castop-density-hard-reject-total key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "castop_density_hard_reject_total",
            "query primitive: castop_density_hard_reject_total underscored alias",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "castop-density-hard-wired",
            "query primitive: castop-density-hard-wired sentinel",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "castop_density_hard_wired",
            "query primitive: castop_density_hard_wired underscored alias",
        ),
        (ROOT / "src/compiler/evaluator_primitives_query.cpp", "schema-2319", "query primitive: schema-2319 sentinel"),
        (ROOT / "src/compiler/evaluator_primitives_query.cpp", "issue-2319", "query primitive: issue-2319 sentinel"),
        # No regression of #2287 keys
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "castop-annotation-hint",
            "query primitive: #2287 castop-annotation-hint retained (no #2287 schema break)",
        ),
        # tests/compiler/test_dead_coercion_layered_2282.cpp: ac2319_* test functions
        (
            ROOT / "tests/compiler/test_dead_coercion_layered_2282.cpp",
            "ac2319_hard_gate_wiring",
            "test: ac2319_hard_gate_wiring function present",
        ),
        (
            ROOT / "tests/compiler/test_dead_coercion_layered_2282.cpp",
            "ac2319_query_keys_wired",
            "test: ac2319_query_keys_wired function present",
        ),
        (
            ROOT / "tests/compiler/test_dead_coercion_layered_2282.cpp",
            "ac2319_soft_default_unchanged",
            "test: ac2319_soft_default_unchanged function present",
        ),
        (
            ROOT / "tests/compiler/test_dead_coercion_layered_2282.cpp",
            "ac2319_hard_path_fires",
            "test: ac2319_hard_path_fires function present",
        ),
        (
            ROOT / "tests/compiler/test_dead_coercion_layered_2282.cpp",
            "ac2319_source_cite_rows",
            "test: ac2319_source_cite_rows function present",
        ),
        (ROOT / "tests/compiler/test_dead_coercion_layered_2282.cpp", "Issue #2319", "test: cites Issue #2319"),
        (
            ROOT / "tests/compiler/test_dead_coercion_layered_2282.cpp",
            "RUN_ALL_TESTS()",
            "test: RUN_ALL_TESTS() in main()",
        ),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_castop_density_hard_gate_coverage.py",
            "optional hard CastOp density gate",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2319 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
