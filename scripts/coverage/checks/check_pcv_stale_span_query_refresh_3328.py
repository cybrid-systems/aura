#!/usr/bin/env python3
"""Issue #3328: production children_stable / query re-use refreshes stale span.

#3167 made SafePCVSpan fingerprint + is_stale observable and wired
force_refresh_pcv_span. Production Agent paths that return or re-export
children_stable / SafePCVSpan after a structural Guard did not call it,
so a held span could silently walk pre-mutate COW storage. #3328 is the
read/export face: production force_refresh or structured stale-span /
across-guard. Soft keeps is_stale + #3167 counter only. #2906 / #3233
mutate exclusive policy is unchanged.

Contract:
  AC1 production held span across Guard → live children or stale-span
  AC2 Soft / Off → frozen view; happy path zero extra
  AC3 #2906 exclusive + #3233 force-exclusive non-regressing
  AC4 extend #3167 / PCV fixture; no test_issue_*.cpp; no docs/design
  AC5 source-cite production children_stable / query re-use path

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

    hh = _read("src/core/persistent_child_vector.hh")
    ast = _read("src/core/ast.ixx")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    t = _read("tests/core/test_pcv_exclusive_with_set.cpp")
    hyg = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    must("kPcvSpanQueryRefreshIssue = 3328", "AC1 stamp", hh)
    must("pcv_span_for_agent_export", "AC1 helper", ast)
    must("force_refresh_pcv_span", "AC1 refresh", ast)
    must("ac3328_1_production_held_span_refresh", "AC1 test", t)

    must("if (!production)", "AC2 Soft identity", ast)
    must("ac3328_2_soft_frozen_view", "AC2 test", t)

    must("kPcvFlatastLockedExclusiveIssue = 2906", "AC3 2906", hh)
    must("kPcvStaleSpanExclusiveIssue = 3233", "AC3 3233", hh)
    must("ac3328_3_2906_3233_non_regression", "AC3 test", t)
    must("ac3328_3_2906_3233_non_regression", "AC3 hygiene", hyg)

    must("Issue #3328", "AC4 query cite", qws)
    must("Issue #3328", "AC4 batch cite", fiber)
    must("pcv_span_stale_across_guard_total", "AC4 reuse 3167 counter", ast)

    must("stale-span", "AC5 structured error", qws)
    must("across-guard", "AC5 reason", qws)
    must("force_refresh_pcv_span", "AC5 query refresh", qws)
    must("pcv_span_for_agent_export", "AC5 query pin", qws)
    must("pcv_span_for_agent_export", "AC5 children_stable_batch", fiber)
    must("query:children-stable", "AC5 children-stable prim", qws)
    must("ac3328_5_source_and_linter", "AC5 hygiene", hyg)
    must("check_pcv_stale_span_query_refresh_3328", "AC5 build.py", build)

    prev3167 = build.find("check_pcv_span_stale_coverage_3167")
    prev3233 = build.find("check_pcv_stale_span_exclusive_3233")
    ours = build.find("check_pcv_stale_span_query_refresh_3328")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev3233 >= 0 and ours < prev3233:
        fails.append("AC5: linter must be wired in build.py AFTER #3233")
    if prev3167 < 0:
        fails.append("AC5: #3167 linter missing (lineage)")

    if "g_3328_" in hh or "g_3328_" in ast:
        fails.append("AC5: new g_3328_* counter")
    if (ROOT / "tests" / "issues" / "test_issue_3328.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3328.cpp per #81967")
    if (ROOT / "tests" / "core" / "test_issue_3328.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3328.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3328.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3328.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3328-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3328 pcv_stale_span_query_refresh:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3328 pcv_stale_span_query_refresh: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
