#!/usr/bin/env python3
"""Issue #3360: Soft TIMEOUT half-solved must not become query:type authority.

Residual of #3331/#3203/#3237: Soft + allow_timeout_commit quarantines
roots but live OccurrenceGoals could still be Agent-visible. Refuse the
export face (g_type_export_soft_refuse_observe_total) and discard
provisional OccurrenceGoals. Soft commit for iteration stays.
Production fail-closed unchanged.

Contract:
  AC1 Soft TIMEOUT → empty provisional goals + not query:type authority
  AC2 Production TIMEOUT fail-closed / #3169 hard clear unchanged
  AC3 Soft still exports TIMEOUT (iteration); only export face refused
  AC4 this linter after #3294; extend test_solve_delta_unresolved_export;
      no invent / docs/design / g_3360_* / schema-3360

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
    aud = _read("src/compiler/typed_mutation_audit.h")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )
    build = _read("build.py")

    must("kSoftTimeoutExportRefuseIssue = 3360", "AC1 stamp", ixx)
    must("discard_provisional_occurrence_goals", "AC1 discard", impl)
    must("g_type_export_soft_refuse_observe_total", "AC1 refuse counter", impl)
    must("ac3360_1_soft_timeout_empty_goals_not_authoritative", "AC1 test", t)
    helper_pos = impl.find("void ConstraintSystem::soft_quarantine_partial_goals_after_timeout() noexcept")
    helper = impl[helper_pos : helper_pos + 2200] if helper_pos >= 0 else ""
    must("discard_provisional_occurrence_goals", "AC1 helper discard", helper)
    must("!occurrence_goals_.empty()", "AC1 have includes goals", helper)
    if "occurrence_persist_log_" in helper:
        fails.append("AC1: helper must not touch occurrence_persist_log_")

    must("delta_timeout_fail_closed_total", "AC2 #3003", impl)
    must("clear_partial_goals_and_unresolved();", "AC2 #3169", impl)
    must("ac3360_2_production_unchanged", "AC2 test", t)

    must("return SolveResult::TIMEOUT; // AC1: not SOLVED", "AC3 keep TIMEOUT", impl)
    must("Issue #3360", "AC3 Evaluator", ev)
    must("g_type_export_soft_refuse_observe_total", "AC3 reuse refuse", aud)
    must("ac3360_3_soft_commit_iteration_ok", "AC3 test", t)

    must("check_soft_timeout_export_refuse_3360", "AC4 build.py", build)
    must("ac3360_4_source_and_linter", "AC4 test", t)
    prev = build.find("check_type_export_outermost_face_3294")
    ours = build.find("check_soft_timeout_export_refuse_3360")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3294")
    if "schema-3360" in q:
        fails.append("AC4: new schema-3360 query key")
    if "g_3360_" in impl or "g_3360_" in ixx or "g_3360_" in ev:
        fails.append("AC4: new g_3360_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3360.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3360.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3360.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3360.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3360-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3360 soft_timeout_export_refuse:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3360 soft_timeout_export_refuse: empty goals + export refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
