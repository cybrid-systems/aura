#!/usr/bin/env python3
"""Issue #2983: production default required TypeId set on composite_txn_commit.

Contract:
  AC1 production + empty span + touched → auto-fill + check (or reject 14)
  AC2 Soft + empty span → zero cost; no auto-fill
  AC3 explicit non-empty span unchanged (#2898)
  AC4 cap ≤16
  AC5 additive schema-2983; preserve #2898/#2610/#2851
  AC6 source-cite + extend test_composite_txn_commit; no docs/design/
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

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
    q = read_query_prims()
    t = _read("tests/compiler/test_composite_txn_commit.cpp")
    build = _read("build.py")

    must("Issue #2983", "AC1", aud)
    must("composite_required_type_auto_fill_total", "AC1", aud)
    must("kCompositeRequiredTypeAutoFillCap", "AC1", aud)
    must("Issue #2983", "AC1", tc)
    must("set_composite_required_solved", "AC1", tc)
    must("stage_composite_touched_unbound_for_test", "AC1", tc)
    must("ac2983_1_prod_empty_span_autofill_rejects", "AC1", t)

    must("zero cost", "AC2", t)
    must("apply_dev_audit_defaults", "AC2", t)
    must("ac2983_2_soft_empty_zero_cost", "AC2", t)

    must("ac2983_3_explicit_span_unchanged", "AC3", t)
    must("stage_composite_required_unbound_var_for_test", "AC3", t)
    must("2898", "AC3", t)

    must("kCompositeRequiredTypeAutoFillCap = 16", "AC4", aud)
    must("stage_composite_touched_n_for_test", "AC4", tc)
    must("ac2983_4_cap_sixteen", "AC4", t)
    must("auto_fill_capped_total", "AC4", aud)

    must("schema-2983", "AC5", q)
    must("schema-2898", "AC5", q)
    must("schema-2610", "AC5", q)
    must("composite-required-type-auto-fill-total", "AC5", q)
    must("commit-readiness-force-reason-required-type", "AC5", q)
    must("ac2983_5_additive_query", "AC5", t)

    must("ac2983_6_source_and_linter", "AC6", t)
    must("check_composite_required_type_default_2983", "AC6", build)
    must("composite_txn_commit", "AC6", tc)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2983-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2983.cpp").is_file():
        fails.append("tests/compiler/test_issue_2983.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2983 production default required TypeId — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
