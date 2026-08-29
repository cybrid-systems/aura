#!/usr/bin/env python3
"""Issue #3359: CoercionMap / CastOp identity elision × abort/densify.

Residual of #3102/#3130/#3347: after identity elision, concurrent
mid-abort or densify/steal can re-grant commit_readiness. Clear the
CoercionMap readiness face at abort-authority enter; elision predicates
re-sample mid-abort outstanding + persist seqlock. Soft observe;
Production/Full refuse. No new query key.

Contract:
  AC1 Production/Full: begin_abort/begin_mid clear coercion face;
      linear_move_drop_elision_ok + ir_typed_entry refuse while abort live
  AC2 Soft observe-only; elision not refused
  AC3 reuse mid-abort + #3225 seqlock + #3102 clear; identity skip_stale
  AC4 extend test_coercion_map_abort_rewind; linter after #3347;
      no invent / docs/design / g_3359_* / schema-3359

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    cm = _read("src/compiler/coercion_map.ixx")
    test = _read("tests/compiler/test_coercion_map_abort_rewind.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kCastopAbortElisionInterleaveIssue = 3359", "AC1 stamp", tma)
    must("abort_or_mid_abort_blocks_elision", "AC1 helper", tma)
    must("clear CoercionMap commit-readiness at abort enter", "AC1 begin_abort", tma)
    must("mid-bound abort enter also drops the CoercionMap", "AC1 begin_mid", tma)
    must("re-sample mid-abort outstanding", "AC1 linear_move_drop", tma)
    must("re-sample mid-abort outstanding before granting", "AC1 ir_typed_entry", tma)
    must("test_ac3359_castop_abort_elision_interleave", "AC1 test", test)
    must("3359 AC1: ir_typed_entry refuses (readiness false)", "AC1 fixture", test)

    must("Soft: false (observe-only)", "AC2 helper Soft", tma)
    must("3359 AC2: Soft does not block elision", "AC2 test", test)

    must("g_occurrence_persist_seq", "AC3 seqlock", tma)
    must("mid_abort_authority_outstanding", "AC3 mid-abort", tma)
    must("clear_coercion_commit_readiness_on_abort", "AC3 #3102 clear", tma)
    must("abort_or_mid_abort_blocks_elision", "AC3 identity elision", cm)
    must("skipped_stale", "AC3 refuse → skip_stale", cm)
    if "schema-3359" in q:
        fails.append("AC3: new schema-3359 query key")
    if "g_3359_" in tma or "g_3359_" in cm:
        fails.append("AC3: new g_3359_* counter")

    must("check_castop_abort_elision_interleave_3359", "AC4 build.py", build)
    must("3359 AC4: linter present", "AC4 test", test)
    prev = build.find("check_residual_castop_readiness_undermark_3347")
    ours = build.find("check_castop_abort_elision_interleave_3359")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3347")
    if (ROOT / "tests" / "compiler" / "test_issue_3359.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3359.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3359.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3359.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3359-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3359 castop_abort_elision_interleave:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3359 castop_abort_elision_interleave: abort enter clears + elision refuse")
    return 0


if __name__ == "__main__":
    sys.exit(main())
