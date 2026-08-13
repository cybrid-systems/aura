#!/usr/bin/env python3
"""Issue #2981: steal/densify rehydrate miss binds TypeLinearCommitProof same-txn.

#2704 latches occurrence_empty_after_fence; densify/steal with_outcome
success stamps could still publish would_allow_commit=true with empty
goals. Same-exit proof must reject (force_reason 11) under production/Full.

Contract:
  AC1 Production/Full + rehydrate miss → would_allow_commit=false / reason 11
  AC2 Soft + miss → soft counter only; no hard proof reject
  AC3 Quiet (no prune / rehydrate success) → no extra reject
  AC4 #2854 same-txn order preserved (proof after rebind/scan)
  AC5 Additive schema-2981; #2704/#2910/#2842/#2697 preserved
  AC6 Source-cite + extend persist-rehydrate + type-linear-commit suites;
      no invent; no docs/design/*

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    ixx = _read("src/compiler/type_checker.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    qts = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    qrf = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    persist = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2981", "AC1", tma)
    must("occurrence_empty_after_fence_blocks_proof", "AC1", tma)
    must("g_type_linear_proof_reject_empty_after_fence_total", "AC1", tma)
    must("Issue #2981", "AC1 fence", ixx)
    must("would_allow_commit=*/false", "AC1 fence stamp", ixx)
    must("empty_fence_2981", "AC1 densify", mb)
    must("empty_fence_2981", "AC1 steal", efm)
    must("ac2981_1_prod_miss_rejects_proof", "AC1", persist)

    # AC2
    must("Soft: observe only", "AC2", tma)
    must("ac2981_2_soft_observe_only", "AC2", persist)

    # AC3
    must("live_goal_count != 0", "AC3", tma)
    must("ac2981_3_quiet_zero_extra", "AC3", persist)

    # AC4
    must("note_type_freshness_after_steal_or_densify", "AC4", mb)
    must("empty_fence_2981", "AC4 order", mb)
    must("ac2981_4_same_txn_order", "AC4", persist)

    # AC5
    must("schema-2981", "AC5 fidelity", qts)
    must("schema-2981", "AC5 health", qrf)
    must("schema-2704", "AC5 #2704", qts)
    must("schema-2910", "AC5 #2910", qts)
    must("schema-2613", "AC5 #2613", qrf)
    must("schema-2842", "AC5 #2842", qrf)
    must("ac2981_5_additive_schema", "AC5", persist)
    must("ac2981_2_health_schema", "AC5 health test", health)

    # AC6
    must("Issue #2981", "AC6 tma", tma)
    must("Issue #2981", "AC6 mb", mb)
    must("check_type_linear_proof_empty_after_fence_2981", "AC6", build)
    must("ac2981_6_source_and_linter", "AC6", persist)
    if (ROOT / "tests" / "compiler" / "test_issue_2981.cpp").is_file():
        fails.append("AC6: test_issue_2981.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2981*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2981 empty-after-fence same-txn proof bind — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
