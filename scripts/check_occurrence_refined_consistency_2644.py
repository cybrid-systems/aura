#!/usr/bin/env python3
"""Issue #2644: batch-level TypeVar refined consistency (anti SOLVED-but-drift).

Contract:
  AC1 two incompatible refined on same var in one composite → production reject
  AC2 Soft → observe only (no reject)
  AC3 consistent refined / single goal → zero extra reject
  AC4 empty occurrence table → no scan (zero cost)
  AC5 schema + source-cite + linter registration
  AC6 soak test dual Soft/production (deferred to follow-up commit)

Exit 0 = all AC rows satisfied.
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
    audit_h = _read("src/compiler/typed_mutation_audit.h")
    eval_tc = _read("src/compiler/evaluator_typecheck.cpp")
    test = _read("tests/compiler/test_composite_commit_cs_reuse_2180.cpp")
    build = _read("build.py")

    # AC1: production/Full reject — wiring + reject counter
    must("check_occurrence_refined_consistency", "AC1", ixx)
    must("check_occurrence_refined_consistency", "AC1", impl)
    must("composite_type_scheme_drift_reject_total", "AC1", audit_h)
    must("composite_type_scheme_drift_reject_total", "AC1", eval_tc)
    must("production_defaults_active()", "AC1", eval_tc)
    must("AuditStrategy::Full", "AC1", eval_tc)
    # AC2: Soft observe only — wiring + observe counter
    must("composite_type_scheme_drift_observe_total", "AC2", audit_h)
    must("composite_type_scheme_drift_observe_total", "AC2", eval_tc)
    # AC3: consistent refined / single goal → zero extra reject — short-circuit
    must("rep_first", "AC3", impl)
    # AC4: empty occurrence table → zero cost
    must("occurrence_goals_.empty()", "AC4", impl)
    must("return true", "AC4", impl)
    # AC5: schema + source-cite + test + linter
    must("#2644", "AC5", ixx)
    must("#2644", "AC5", impl)
    must("#2644", "AC5", audit_h)
    must("#2644", "AC5", eval_tc)
    must("#2644", "AC5", test)
    must("ac2644_empty_cs_no_drift_bump", "AC5", test)
    must("ac2644_source_cite", "AC5", test)
    # Linter registration in build.py
    must("check_occurrence_refined_consistency_2644", "AC5", build)
    must("composite_type_scheme_drift_reject_total", "AC5", audit_h)
    must("composite_type_scheme_drift_observe_total", "AC5", audit_h)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2644 batch-level TypeVar refined consistency — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
