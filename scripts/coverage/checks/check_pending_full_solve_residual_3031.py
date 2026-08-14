#!/usr/bin/env python3
"""Issue #3031: pending_full_solve + locality residual drain before commit.

Production/Full composite_txn_commit escalates residual then hard-rejects
(force_reason pending_full_solve_residual / 16) if still dirty. Soft
observe-only. Quiet (no residual) is two size reads, no extra atomics.

Contract:
  AC1 Production pending/locality → drain escalate; still dirty → reject
  AC2 Soft: observe allow
  AC3 Quiet residual 0: no extra counters
  AC4 commit_readiness hermetic force_reason 16
  AC5 schema-3031 + residual keys on fidelity-stats + type-linear health
  AC6 extend test_solve_delta_unresolved_export; no docs/design / invent

Exit 0 = all rows satisfied.
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

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    ev = _read("src/compiler/evaluator_typecheck.cpp")
    q = read_query_prims()
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    th = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    must("drain_pending_full_solve_before_commit", "AC1", ixx)
    must("drain_pending_full_solve_before_commit", "AC1", impl)
    must("drain_pending_full_solve_before_commit", "AC1", ev)
    must("escalate_if_production(SolveResult::TIMEOUT", "AC1", impl)
    must("ac3031_1_production_drain_escalate_reject", "AC1", t)
    must("/*force_reason=*/16", "AC1", ev)

    must("ac3031_2_soft_observe_allow", "AC2", t)
    must("g_pending_full_solve_residual_observe_total", "AC2", impl)

    must("ac3031_3_quiet_zero_cost", "AC3", t)
    must("pending == 0 && loc == 0", "AC3", impl)

    must("ac3031_4_commit_readiness_hermetic", "AC4", t)
    must("pending_full_solve_residual", "AC4", aud)
    must("return 16", "AC4", aud)

    must("ac3031_5_schema", "AC5", t)
    must("ac3031_health_schema", "AC5", th)
    must_key("schema-3031", "AC5", q)
    must_key("pending-full-solve-residual-last", "AC5", q)
    must_key("pending-full-solve-residual-observe-total", "AC5", q)
    must_key("pending-full-solve-residual-escalate-total", "AC5", q)
    must_key("pending-full-solve-residual-reject-total", "AC5", q)
    must_key("pending-full-solve-residual-wired", "AC5", q)
    must("kPendingFullSolveResidualIssue", "AC5", aud)

    must("ac3031_6_source_and_linter", "AC6", t)
    must("check_pending_full_solve_residual_3031", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3031.cpp").is_file():
        fails.append("AC6: test_issue_3031.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3031-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3031 pending_full_solve residual — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
