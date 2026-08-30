#!/usr/bin/env python3
"""Issue #3451: nested_authority_gap poisons QueryEpoch / held QueryResult.

#3312 closed the query:*-stable export window (thin hot-cone + gap).
Held QueryResult / QueryEpoch::is_fresh still compared mutation_epoch +
generation only. Production nested success now reuses #3041
force_query_epoch_stale_from_restamp_budget after note_nested_authority_gap,
and query_result_is_fresh_with_refs production-gates the gap check with
note_query_result_stale. Soft / Off: zero extra. No new query key.

Contract:
  AC1  Production nested success → with_refs stale + last_query_epoch stale
  AC2  nested-touched query:*-stable still follows #3312
  AC3  outermost clears gap; new capture fresh; pre-nested QR stays stale
  AC4  Soft / Off: no poison call, no extra stale atomic
  AC5  extend hygiene + incremental restamp; linter after #3312; no invent

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    dec = _read("src/compiler/query_result_decode.hh")
    qw = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    we = _read("src/core/workspace_epoch.hh")
    hyg = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    rest = _read("tests/core/test_incremental_restamp.cpp")
    l3312 = _read("scripts/coverage/checks/check_nested_return_not_triad_3312.py")
    build = _read("build.py")

    nest = mb.find("Issue #3312: production nested success may thin-hot-cone")
    nwin = mb[nest : nest + 2800] if nest >= 0 else ""
    must("Issue #3451", "AC1 boundary cite", nwin)
    must("note_nested_authority_gap", "AC1 gap note", nwin)
    must("force_query_epoch_stale_from_restamp_budget", "AC1 reuse #3041", nwin)
    gap = nwin.find("note_nested_authority_gap")
    poison = nwin.find("force_query_epoch_stale_from_restamp_budget")
    if gap < 0 or poison < 0 or poison < gap:
        fails.append("AC1: force_query_epoch_stale_from_restamp_budget must follow note_nested_authority_gap")
    if "unified_restamp_after_boundary(" in nwin:
        fails.append("AC1: nested #3451 block calls unified_restamp_after_boundary")
    must("ac3451_1_nested_held_query_result_stale", "AC1 test", hyg)

    must("allow_query_stable_ref_export", "AC2 #3312 export kept", hyg)
    must("ac3451_2_nested_touched_export_unchanged", "AC2 test", hyg)
    must("restamp_hot_cone_after_budget", "AC2 thin-cone kept", nwin)

    must("clear_nested_authority_gap", "AC3 outermost clear", mb)
    must("ac3451_3_outermost_clears_new_capture_fresh", "AC3 test", hyg)
    must("nested abort does not poison", "AC3 abort", hyg)

    must("Soft / Off never reach this arm", "AC4 poison gated", mb)
    must("ac3451_4_soft_zero_extra", "AC4 test", hyg)
    must("production_defaults_active()", "AC4 decode production-gate", dec)
    # Soft decode path must not bump stale on the gap face without hard.
    dfun = dec.find("query_result_is_fresh_with_refs")
    dwin = dec[dfun : dfun + 1600] if dfun >= 0 else ""
    must("hard && flat.nested_authority_gap()", "AC4 gated gap", dwin)
    must("note_query_result_stale", "AC4 stale bump", dwin)

    must("Issue #3451", "AC5 query_workspace cite", qw)
    must("kNestedGapQueryEpochStaleIssue = 3451", "AC5 stamp", we)
    must("ac3451_5_source_and_linter", "AC5 hygiene source", hyg)
    must("3451", "AC5 incremental restamp", rest)
    must("check_nested_gap_query_epoch_stale_3451", "AC5 build.py", build)
    must("force_query_epoch_stale_from_restamp_budget", "AC5 extend #3312 linter", l3312)
    prev = build.find("check_nested_return_not_triad_3312")
    ours = build.find("check_nested_gap_query_epoch_stale_3451")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3312")
    must_not("schema-3451", "AC5 no new query key", mb)
    must_not("schema-3451", "AC5 no new query key decode", dec)
    if (ROOT / "tests" / "issues" / "test_issue_3451.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3451.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3451.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3451.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3451-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3451 nested_gap_query_epoch_stale:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3451 nested_gap_query_epoch_stale: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
