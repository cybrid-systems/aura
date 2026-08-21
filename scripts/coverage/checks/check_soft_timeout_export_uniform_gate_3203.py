#!/usr/bin/env python3
"""Issue #3203: Soft TIMEOUT/CONFLICT must never grant query:type authority.

Residual of #3081: persist outermost grant could stamp true over a
half-solved Soft TIMEOUT face, and last_occurrence_vars still leaked
refined TypeIds. Uniform helper refuses unless last face is SOLVED.

Contract:
  AC1 Soft TIMEOUT + grant → not-authoritative; no live TypeId
  AC2 Production TIMEOUT fail-closed unchanged (#3003 / #3169)
  AC3 Outermost SOLVED still grants; nested inflight does not
  AC4 Quiet SOLVED: solve-status check first, no production_defaults load
  AC5 this linter + test_solve_delta_unresolved_export; no docs/design / invent

Exit 0 = all rows satisfied.
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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    prim = _read("src/compiler/evaluator_primitives_eval.cpp")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    must("kSoftTimeoutExportUniformGateIssue = 3203", "AC1 stamp", ixx)
    must("type_export_is_authoritative", "AC1 TC helper", ixx)
    must("type_export_is_authoritative", "AC1 Evaluator helper", ev)
    must("last_type_solve_solved_", "AC1 solve-solved latch", ev)
    must("if (!last_type_solve_solved_)", "AC1 grant refuse", ev)
    must("ac3203_1_grant_cannot_override_timeout", "AC1 test", t)

    must("delta_timeout_fail_closed_total", "AC2 #3003", impl)
    must("delta_timeout_reject_total", "AC2 #2277", impl)
    must("ac3203_2_production_timeout_unchanged", "AC2 test", t)

    must("grant_type_export_authority", "AC3 persist grant", mb)
    must("note_type_export_inflight", "AC3 nested inflight", ev)
    must("ac3203_3_solved_still_grants", "AC3 test", t)

    # AC4: helper compares solve first; no production_defaults on quiet path
    must("if (!last_type_solve_solved_)", "AC4 Evaluator first check", ev)
    must("if (last_delta_solve_status_ != SolveResult::SOLVED)", "AC4 TC first check", ixx)
    must("note_infer_solve_solved", "AC4 typecheck plumb", tc)
    must("ac3203_4_quiet_solved_zero_cost", "AC4 test", t)
    h = ev.find("bool type_export_is_authoritative() const noexcept")
    if h < 0:
        fails.append("AC4: Evaluator type_export_is_authoritative missing")
    else:
        end = ev.find("}", h)
        body = ev[h:end] if end > h else ""
        if "production_defaults_active" in body:
            fails.append("AC4: production_defaults_active on quiet SOLVED helper")
        if "last_type_solve_solved_" not in body:
            fails.append("AC4: helper must consult last_type_solve_solved_ first")

    must("Issue #3203", "AC5 occurrence clear", impl)
    must("last_occurrence_vars_.clear()", "AC5 no live TypeId", impl)
    must("not-authoritative", "AC5 suggest/query", prim)
    must("check_soft_timeout_export_uniform_gate_3203", "AC5 build.py", build)
    must("ac3203_5_source_and_linter", "AC5 test", t)

    if "query:soft-timeout-uniform-gate" in ev or "query:soft-timeout-uniform-gate" in ixx:
        fails.append("AC5: new public query key")
    if "g_3203_" in ev or "g_3203_" in ixx:
        fails.append("AC5: invented g_3203_* counter")

    if (ROOT / "tests" / "compiler" / "test_issue_3203.cpp").is_file():
        fails.append("AC5: test_issue_3203.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3203-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3203 Soft TIMEOUT export uniform gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
