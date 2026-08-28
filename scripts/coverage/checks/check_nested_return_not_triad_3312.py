#!/usr/bin/env python3
"""Issue #3312: nested Guard success is never triad-complete.

Production nested success still publishes the #3196 authority-gap face
(outermost owns unified_restamp + LayoutStamp + Phase-5). When the nested
mutation_log delta fits the #3259 hot-cone cap, nested-touched nodes get a
thin restamp_hot_cone_after_budget so query:*-stable can export those nodes
only. Soft / Off: zero extra. No docs/design / invent.

Contract:
  AC1  production nested-touched → eager + exportable; outside cone gap
  AC2  Soft / Off → zero extra beyond #3166 observe
  AC3  outermost triad + nested abort unchanged
  AC4  never green pre-mutate gen
  AC5  extend hygiene_mutate_closed_loop + mutation_boundary_batch;
       linter after #3196; no invent

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

    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    sec = _read("src/compiler/evaluator_security.cpp")
    ast = _read("src/core/ast.ixx")
    met = _read("src/compiler/observability_metrics.h")
    hyg = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    batch = _read("tests/compiler/test_mutation_boundary_batch.cpp")
    build = _read("build.py")
    l3196 = _read("scripts/coverage/checks/check_nested_guard_authority_gap_3196.py")

    must("Issue #3312", "AC1 boundary", mb)
    must("restamp_hot_cone_after_budget", "AC1 reuse collector", mb)
    must("clear_restamp_eager_bits", "AC1 drop full-tree eager", mb)
    must("nested_return_not_triad_complete", "AC1 last-authority", mb)
    must("node_eagerly_restamped", "AC1 export eager", sec)
    must("ac3312_1_nested_hot_cone_or_gap", "AC1 test", hyg)

    nest = mb.find("Issue #3312: production nested success may thin-hot-cone")
    nwin = mb[nest : nest + 1800] if nest >= 0 else ""
    if nest >= 0 and "unified_restamp_after_boundary(" in nwin:
        fails.append("AC1: nested #3312 block calls unified_restamp_after_boundary")

    must("ac3312_2_soft_zero_extra", "AC2 test", hyg)
    must("Soft / Off: zero extra", "AC2 comment", mb)

    must("clear_nested_authority_gap", "AC3 outermost clear", mb)
    must("unified_restamp_after_boundary", "AC3 outermost triad", mb)
    must("ac3312_3_outermost_and_abort_unchanged", "AC3 test", hyg)

    must("ac3312_4_never_green_pre_mutate", "AC4 test", hyg)
    must("never green a pre-mutate gen", "AC4 fail-closed", sec)

    must("kNestedReturnNotTriadIssue = 3312", "AC5 stamp", met)
    must("nested_hot_cone_restamp_total", "AC5 hot-cone total", met)
    must("nested_authority_gap_last_window_ns", "AC5 window", met)
    must("nested_authority_gap_open_ns", "AC5 open stamp", ast)
    must("ac3312_5_source_and_linter", "AC5 hygiene source", hyg)
    must("check_nested_return_not_triad_3312", "AC5 build.py", build)
    must("run_3196_nested_authority_gap", "AC5 batch", batch)
    prev = build.find("check_nested_guard_authority_gap_3196")
    ours = build.find("check_nested_return_not_triad_3312")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3196")
    if "schema-3312" in hyg or "schema-3312" in met:
        fails.append("AC5: new schema-3312 query key (SlimSurface)")
    if "g_3312_" in mb or "g_3312_" in met or "g_3312_" in ast:
        fails.append("AC5: new g_3312_* counter")
    if (ROOT / "tests" / "issues" / "test_issue_3312.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3312.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3312.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3312.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3312-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    must("3312", "AC5 extend 3196 linter", l3196)

    if fails:
        print("FAIL #3312 nested_return_not_triad:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3312 nested_return_not_triad: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
