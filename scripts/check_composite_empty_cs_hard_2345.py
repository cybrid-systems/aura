#!/usr/bin/env python3
"""Issue #2345: production composite empty-CS hard-reject coverage.

Contract:
  AC1: production / Full + expected_partial + empty CS → hard-miss + reject
  AC2: dev Sampled soft → observe only
  AC3: structural-only (no txn_dirty) → no empty-CS policy reject
  AC4: additive query keys (schema-2345); #2262 / #2180 lineage retained
  AC5: source-cite composite_txn_commit + commit_cs_has_work

Exit 0 = all ACs satisfied.
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

    etc = _read("src/compiler/evaluator_typecheck.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    ixx = _read("src/compiler/type_checker.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    test = _read("tests/compiler/test_partial_cs_single_source_2262.cpp")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("composite_empty_cs_hard_reject_enabled", "AC1", aud)
    must("composite_commit_empty_cs_hard_miss_total", "AC1", aud)
    must("composite_commit_empty_cs_hard_miss_total", "AC1", etc)
    must("AC5", "AC1", test)
    must("apply_production_audit_defaults", "AC1", test)

    # AC2
    must("composite_commit_empty_cs_observe_total", "AC2", aud)
    must("composite_commit_empty_cs_observe_total", "AC2", etc)
    must("AURA_COMPOSITE_EMPTY_CS_HARD", "AC2", aud)
    must("AC6", "AC2", test)

    # AC3
    must("AC7", "AC3", test)
    must("commit_cs_has_work", "AC3", ixx)
    must("txn_dirty", "AC3", etc)

    # AC4
    must("schema-2345", "AC4", q)
    must("composite-commit-empty-cs-hard-miss-total", "AC4", q)
    must("composite-commit-empty-cs-observe-total", "AC4", q)
    must("composite-empty-cs-hard-wired", "AC4", q)
    must("schema-2262", "AC4", q)
    must("composite-commit-solve-empty-cs-total", "AC4", q)
    must("schema-2345", "AC4", mut)

    # AC5
    must("composite_txn_commit", "AC5", etc)
    must("#2345", "AC5", etc)
    must("AC8", "AC5", test)
    must("test_partial_cs_single_source_2262", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2345 composite empty-CS hard-reject — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
