#!/usr/bin/env python3
"""Issue #2898: explicit required TypeId invariant set on composite_txn_commit.

Contract:
  AC1 production + unbound required → reject + fail_total
  AC2 empty required span → zero cost
  AC3 Soft miss → observe only (not hard fail)
  AC4 additive query keys + schema-2898; preserve #2105/#2610/#2851
  AC5 source-cite + extend test_composite_txn_commit; no docs/design/
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    aud = _read("src/compiler/typed_mutation_audit.h")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    om = _read("src/compiler/observability_metrics.h")
    t = _read("tests/compiler/test_composite_txn_commit.cpp")
    build = _read("build.py")

    must("2898", "AC1", aud)
    must("composite_required_type_fail_total", "AC1", aud)
    must("composite_required_type_observe_total", "AC1", aud)
    must("set_composite_required_solved", "AC1", aud)
    must("composite_required_type_fail_total", "AC1", tc)
    must("required_type_ok", "AC1", aud)
    must("stage_composite_required_unbound_var_for_test", "AC1", tc)

    must("empty", "AC2", tc)  # AC2 documents empty span zero cost
    must("composite_required_type_checked_total", "AC2", aud)

    must("observe", "AC3", tc)
    must("Soft", "AC3", tc)
    must("required_type", "AC3", aud)

    must("schema-2898", "AC4", q)
    must("composite-required-type-fail-total", "AC4", q)
    must("composite-required-type-wired", "AC4", q)
    must("schema-2610", "AC4", q)
    must("composite_txn_commit", "AC4", tc)
    must("required_type", "AC4", aud)  # force reason

    must("ac2898_1_production_required_miss_rejects", "AC5", t)
    must("ac2898_2_empty_required_zero_cost", "AC5", t)
    must("ac2898_3_soft_observe_only", "AC5", t)
    must("ac2898_4_additive_query", "AC5", t)
    must("ac2898_5_source_cite", "AC5", t)
    must("check_composite_required_type_2898", "AC5", build)
    must("composite_required_type_fail_total", "AC5", om)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2898-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2898.cpp").is_file():
        fails.append("tests/compiler/test_issue_2898.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2898 composite required TypeId — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
