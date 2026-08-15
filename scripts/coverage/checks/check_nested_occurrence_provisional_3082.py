#!/usr/bin/env python3
"""Issue #3082: mid/nested MutationBoundary occurrence is provisional.

Nested/mid success never writes the durable Occurrence persist log.
While a nested Guard is open, and after nested success, query:type
stays in-flight / non-authoritative until outermost persist grants.
Soft empty / no nested: no extra persist. Outermost persist unchanged.

Contract (one row per AC):
  AC1 Nested/mid success never calls append_occurrence_snapshot
  AC2 Nested open + typecheck copy refuse grant; query in-flight
  AC3 Outermost persist → stamp → ensure → grant unchanged
  AC4 Soft empty / no nested: persist off; inflight only on !outermost
  AC5 Extend test_occurrence_goal_persist_rehydrate; this linter; no invent

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

    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    prim = _read("src/compiler/evaluator_primitives_eval.cpp")
    tma = _read("src/compiler/typed_mutation_audit.h")
    ixx = _read("src/compiler/type_checker.ixx")
    q = read_query_prims()
    test = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    must("kNestedOccurrenceProvisionalIssue = 3082", "AC1 stamp", tma)
    must("Issue #3082", "AC1 dtor", mb)
    must("aura_outermost_success_persist_occurrence", "AC1 helper", mb)
    must("maybe_persist_occurrence_snapshot", "AC1 maybe_persist", mb)
    must("if (outermost && success)", "AC1 outermost-only persist", mb)
    must("append_occurrence_snapshot", "AC1 append exists", ixx)
    # Durable append is only inside maybe_persist (TypeChecker), not the
    # nested dtor. Nested path stamps inflight instead.
    persist_helper = mb.find('extern "C" void aura_outermost_success_persist_occurrence')
    persist_dtor = mb.find("aura_outermost_success_persist_occurrence(ev_")
    nested_inflight = mb.find("else if (!outermost)")
    if persist_helper < 0 or persist_dtor < 0 or persist_dtor < persist_helper:
        fails.append("AC1: persist helper must be the sole dtor persist call")
    if "append_occurrence_snapshot" in mb:
        fails.append("AC1: dtor must not call append_occurrence_snapshot directly")
    if nested_inflight < 0 or "note_type_export_inflight" not in mb[nested_inflight : nested_inflight + 800]:
        fails.append("AC1: !outermost path must note_type_export_inflight")
    must("ac3082_1_nested_success_never_persists", "AC1 test", test)

    must("copy_infer_type_export_authority", "AC2 helper", ev)
    must("copy_infer_type_export_authority", "AC2 typecheck", tc)
    must("copy_infer_type_export_authority", "AC2 typecheck-current", prim)
    must("note_type_export_inflight", "AC2 enter/exit", mb)
    must("if (!outermost)", "AC2 nested enter", mb)
    must("in-flight", "AC2 query", prim)
    must("type_export_authoritative", "AC2 query gate", prim)
    must("ac3082_2_nested_query_inflight", "AC2 test", test)

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
        fails.append("AC3: order must stay persist → stamp → ensure → grant")
    must("ac3082_3_outermost_persist_unchanged", "AC3 test", test)

    must("Soft / env=0", "AC4 persist zero", mb)
    must("ac3082_4_soft_no_nested_zero_extra", "AC4 test", test)

    must("discard_provisional_occurrence_snapshot", "AC5 outer abort", mb)
    must("outermost && !success", "AC5 outer abort", mb)
    must("ac3082_5_nested_fail_inflight_outer_abort_discards", "AC5 test", test)
    must("ac3082_6_schema_and_linter", "AC5 schema test", test)
    must_key("schema-3082", "AC5 schema", q)
    must_key("nested-occurrence-provisional-wired", "AC5 wired", q)
    must("check_nested_occurrence_provisional_3082", "AC5 build.py", build)
    must("linear_fast_path_ok", "AC5 #2964 lineage", tma)
    if (ROOT / "tests" / "compiler" / "test_issue_3082.cpp").is_file():
        fails.append("AC5: test_issue_3082.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3082-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3082 nested occurrence provisional — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
