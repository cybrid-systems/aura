#!/usr/bin/env python3
"""Issue #3004: occurrence persist atomic with Full audit + query:type.

Persist is a side-effect of outermost success only (#2938). Production
infer SOLVED is in-flight until persist + stamp + ensure grant
query:type authority. Failure discards provisional live goals.

Contract:
  AC1 persist → stamp → ensure → grant_type_export_authority
  AC2 Soft: no durable persist
  AC3 Full fail / !success discards provisional + clears authority
  AC4 schema-3004; preserve #2938/#2910/#2964
  AC5 extend test_occurrence_goal_persist_rehydrate; linter; no docs/design

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    impl = _read("src/compiler/type_checker_impl.cpp")
    ixx = _read("src/compiler/type_checker.ixx")
    q = read_query_prims()
    test = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    must("kOccurrencePersistAuditAtomicIssue = 3004", "AC1", tma)
    must("grant_type_export_authority", "AC1", mb)
    must("grant_type_export_authority", "AC1 ev", ev)
    must("note_type_export_inflight", "AC1", ev)
    must("note_type_export_inflight", "AC1 typecheck", tc)
    persist_pos = mb.find("maybe_persist_occurrence_snapshot")
    stamp_pos = mb.find("build_type_linear_commit_proof_from_live", persist_pos if persist_pos >= 0 else 0)
    ens_pos = mb.find("ensure_occurrence_commit_or_recover", stamp_pos if stamp_pos >= 0 else 0)
    grant_pos = mb.find("grant_type_export_authority", ens_pos if ens_pos >= 0 else 0)
    if (
        persist_pos < 0
        or stamp_pos < 0
        or ens_pos < 0
        or grant_pos < 0
        or not (persist_pos < stamp_pos < ens_pos < grant_pos)
    ):
        fails.append("AC1: order must be persist → stamp → ensure → grant")
    must("ac3004_1_authority_after_persist", "AC1", test)

    must("ac3004_2_soft_no_durable", "AC2", test)
    must("Soft / env=0", "AC2", mb)

    must("discard_provisional_occurrence_goals", "AC3", impl)
    must("discard_provisional_occurrence_snapshot", "AC3", ixx)
    must("discard_provisional_occurrence_snapshot", "AC3 dtor", mb)
    must("clear_type_export_authority", "AC3 dtor", mb)
    must("outermost && !success", "AC3", mb)
    must("ac3004_3_discard_provisional_on_fail", "AC3", test)

    must_key("schema-3004", "AC4", q)
    must_key("occurrence-persist-audit-atomic-wired", "AC4", q)
    must("schema-2938", "AC4 lineage", q)
    must("linear_fast_path_ok", "AC4 #2964", tma)
    must("ac3004_4_schema_and_lineage", "AC4", test)

    must("ac3004_5_source_and_linter", "AC5", test)
    must("check_occurrence_persist_audit_atomic_3004", "AC5", build)
    must("in-flight", "AC5 query", _read("src/compiler/evaluator_primitives_eval.cpp"))
    if (ROOT / "tests" / "compiler" / "test_issue_3004.cpp").is_file():
        fails.append("AC5: test_issue_3004.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3004-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3004 occurrence persist + Full audit atomic — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
