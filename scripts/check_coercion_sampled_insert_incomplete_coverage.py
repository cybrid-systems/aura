#!/usr/bin/env python3
"""
Linter for #2317 / #2620 — Sampled incomplete-insert canary retained.

#2620 unifies Soft/production: incomplete dual never inserts under
strategy!=Off by default. #2317 insert is canary-only via
AURA_COERCION_SAMPLED_INCOMPLETE_INSERT=1. Soft default skips insert
and arms force-Full observe (no hard reject).

Verifies:
  - g_coercion_sampled_insert_incomplete_total still present (canary)
  - coercion_sampled_incomplete_insert_canary + env gate
  - #2620 skip-insert + arm_soft_incomplete_force_full_observe
  - production reject-on-miss unchanged
  - query keys schema-2317 retained
  - tests cite Issue #2317 + ac2317_*

Exit 0 on success, 1 on any failure.
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
        # coercion_map.ixx: new counter
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "g_coercion_sampled_insert_incomplete_total",
            "coercion_map.ixx: new counter present",
        ),
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "g_coercion_sampled_insert_incomplete_total{0};",
            "coercion_map.ixx: new counter atomic init",
        ),
        # coercion_map.ixx: canary + default skip (#2620)
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "AuditStrategy::Sampled",
            "coercion_map.ixx: Sampled strategy check present",
        ),
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "AURA_COERCION_SAMPLED_INCOMPLETE_INSERT",
            "coercion_map.ixx: canary env present",
        ),
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "coercion_sampled_incomplete_insert_canary",
            "coercion_map.ixx: canary helper present",
        ),
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "arm_soft_incomplete_force_full_observe",
            "coercion_map.ixx: #2620 soft force-Full arm present",
        ),
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "note_provenance_miss_for_boundary()",
            "coercion_map.ixx: force-audit note present",
        ),
        # coercion_map.ixx: weak mid ban preserved (#2261)
        (ROOT / "src/compiler/coercion_map.ixx", "never weak mid", "coercion_map.ixx: weak mid ban preserved (#2261)"),
        (ROOT / "src/compiler/coercion_map.ixx", "Issue #2317", "coercion_map.ixx: cites Issue #2317"),
        (ROOT / "src/compiler/coercion_map.ixx", "#2620", "coercion_map.ixx: cites #2620"),
        # production reject unchanged
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "g_coercion_provenance_miss_reject_total",
            "coercion_map.ixx: miss_reject counter preserved",
        ),
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "g_coercion_provenance_sampled_reject_total",
            "coercion_map.ixx: sampled_reject counter preserved",
        ),
        # Full / Strict honesty
        (ROOT / "src/compiler/coercion_map.ixx", "AuditStrategy::Full", "coercion_map.ixx: Full audit check present"),
        (ROOT / "src/compiler/coercion_map.ixx", "if (strict)", "coercion_map.ixx: strict mode check present"),
        # evaluator_primitives_query.cpp: query keys
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "coercion-sampled-insert-incomplete-total",
            "query primitive: coercion-sampled-insert-incomplete-total key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "coercion_sampled_insert_incomplete_total",
            "query primitive: underscored alias key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "coercion-sampled-insert-policy-wired",
            "query primitive: policy-wired sentinel",
        ),
        (ROOT / "src/compiler/evaluator_primitives_query.cpp", "schema-2317", "query primitive: schema-2317"),
        (ROOT / "src/compiler/evaluator_primitives_query.cpp", "issue-2317", "query primitive: issue-2317"),
        # test file
        (ROOT / "tests/compiler/test_coercion_ban_weak_ir_2261.cpp", "Issue #2317", "test file: cites Issue #2317"),
        (
            ROOT / "tests/compiler/test_coercion_ban_weak_ir_2261.cpp",
            "ac2317_sampled_insert_policy",
            "test file: has ac2317_sampled_insert_policy function",
        ),
        (
            ROOT / "tests/compiler/test_coercion_ban_weak_ir_2261.cpp",
            "ac2317_reject_unchanged",
            "test file: has ac2317_reject_unchanged function",
        ),
        (
            ROOT / "tests/compiler/test_coercion_ban_weak_ir_2261.cpp",
            "ac2317_query_keys",
            "test file: has ac2317_query_keys function",
        ),
        (
            ROOT / "tests/compiler/test_coercion_ban_weak_ir_2261.cpp",
            "ac2317_source_cite_rows",
            "test file: has ac2317_source_cite_rows function",
        ),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_coercion_sampled_insert_incomplete_coverage.py",
            "Sampled incomplete provenance must insert",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2317 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
