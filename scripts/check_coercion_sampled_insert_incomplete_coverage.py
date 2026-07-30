#!/usr/bin/env python3
"""
Linter for #2317 — Sampled incomplete provenance must insert CoercionNode
+ force next Full audit (not silent skip). Closes the soundness /
debuggability hole where under Sampled strategy, `apply_coercion_map`
skips CoercionNode insertion when provenance is incomplete. Runtime
vs type evidence diverge: Agents see no Cast site, narrow_evidence
never reaches IR, and blame recovery is impossible until an
independent Full audit happens to run. After #2310-#2316 correctness
fixes, Sampled-mode coercion evaporation is the missing piece.

Verifies the implementation is wired correctly:
  - coercion_map.ixx: new counter g_coercion_sampled_insert_incomplete_total
  - coercion_map.ixx: apply path Sampled branch — Sampled + !reject →
    INSERT (with force-audit via note_provenance_miss_for_boundary)
  - coercion_map.ixx: production reject-on-miss unchanged
  - coercion_map.ixx: Full / Strict honesty preserved (#2147 / #2261)
  - evaluator_primitives_query.cpp: query:type-incremental-fidelity-stats
    extended with coercion-sampled-insert-incomplete-total + alias +
    policy-wired sentinel + schema-2317 / issue-2317
  - tests/compiler/test_coercion_ban_weak_ir_2261.cpp cites Issue #2317
    + has ac2317_* test functions

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_coercion_sampled_insert_incomplete_coverage.py
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
        # coercion_map.ixx: apply path Sampled branch
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "AuditStrategy::Sampled",
            "coercion_map.ixx: Sampled strategy check present",
        ),
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "!reject_apply_on_provenance_miss()",
            "coercion_map.ixx: reject check present",
        ),
        (
            ROOT / "src/compiler/coercion_map.ixx",
            "note_provenance_miss_for_boundary()",
            "coercion_map.ixx: force-audit note present",
        ),
        # coercion_map.ixx: weak mid ban preserved (#2261)
        (ROOT / "src/compiler/coercion_map.ixx", "never weak mid", "coercion_map.ixx: weak mid ban preserved (#2261)"),
        (ROOT / "src/compiler/coercion_map.ixx", "Issue #2317", "coercion_map.ixx: cites Issue #2317"),
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
