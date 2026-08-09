#!/usr/bin/env python3
"""Issue #2758: fill TypeLinearCommitProof live_goal_count + linear_root_count
from real walks (#2717 / #2708 residual).

Contract (one row per AC):
  AC1 After boundary with live linear roots and/or goals, stamped proof
     has real linear_root_count / live_goal_count (not hard-coded 0).
  AC2 Quiet path (empty collect, no goals) → both counts 0; reuses
     collect_linear_or_dirty_roots short-circuit.
  AC3 Last stamped counts queryable for densify/steal drift detect.
  AC4 Additive: #2613/#2697/#2717 preserved; counts-filled-total optional.
  AC5 Source-cite stamp path + collect / goals size; this linter.
  AC6 Extend test_type_linear_commit_health.cpp; no docs/design/*.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    def must_key(n: str, label: str, hay: str) -> None:
        # clang-format may split adjacent string literals; strip quotes/whitespace.
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    # AC1 — real walks.
    must("Issue #2758", "AC1", tma)
    must("linear_or_dirty_roots_count_for_rebind", "AC1", tma)
    must("p.linear_root_count =", "AC1", tma)
    must("p.live_goal_count =", "AC1", tma)
    must_not("#2708 future wire", "AC1 residual removed", tma)
    # #2842 may freeze via freeze_proof_goal_truth_from_type_checker (CS goals);
    # #2758 residual still requires a CS goal size path at stamp sites.
    if "occurrence_goals_size()" not in emb and "occurrence_goals_for_test()" not in emb:
        fails.append("AC1: missing occurrence_goals_size() or occurrence_goals_for_test()")
    if "build_type_linear_commit_proof_from_live" not in emb:
        fails.append("AC1: missing build_type_linear_commit_proof_from_live")
    orb = _read("src/compiler/ownership_rebind.h")
    orc = _read("src/compiler/ownership_rebind.cpp")
    must("linear_or_dirty_roots_count_for_rebind", "AC1", orb)
    must("collect_linear_or_dirty_roots_for_rebind().size()", "AC1", orc)

    # AC2 — quiet path.
    must("linear_or_dirty_roots_count_for_rebind", "AC2", tma)
    must("g_proof_live_goal_count_gauge", "AC2", tma)

    # AC3 — last counts.
    must("g_last_proof_live_goal_count", "AC3", tma)
    must("g_last_proof_linear_root_count", "AC3", tma)
    must("last_proof_live_goal_count_v_read", "AC3", q)
    must("last_proof_linear_root_count_v_read", "AC3", q)

    # AC4 — additive.
    must_key("schema-2697", "AC4", q)
    must_key("schema-2717", "AC4", q)
    must_key("type-linear-commit-proof-stamped-total", "AC4", q)
    must_key("type-linear-commit-proof-counts-filled-total", "AC4", q)
    must_key("schema-2758", "AC4", q)
    must_key("issue-2758", "AC4", q)
    must("g_type_linear_commit_proof_counts_filled_total", "AC4", tma)

    # AC5/AC6 — tests + linter + no docs.
    must("ac2758_1_counts_from_real_walks", "AC5", t)
    must("ac2758_2_quiet_path_zeros", "AC5", t)
    must("ac2758_3_last_counts_queryable", "AC5", t)
    must("ac2758_4_additive_no_regression", "AC5", t)
    must("ac2758_5_source_and_linter", "AC5", t)
    must("ac2717_1_boundary_success_and_reject_stamp", "AC5 #2717 preserved", t)
    must("check_type_linear_commit_proof_counts_2758", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2758.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2758.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2758-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2758 TypeLinearCommitProof counts from real walks — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
