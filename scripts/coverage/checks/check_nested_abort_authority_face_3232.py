#!/usr/bin/env python3
"""Issue #3232: nested AbortAuthorityHold keeps one authority face.

Residual of #3193: in_flight was a 0/1 store, so an inner abort end could
drop the face while outer/concurrent abort was still restoring dual
topology. Production/Full counts nested holds; last end drops the face.
Rehydrate still consults abort_authority_blocks_rehydrate. Soft observe;
quiet (no abort) zero extra. Reuses #3193 counters; no new mid-struct key.

Contract:
  AC1 Nested production hold: inner end does not drop outer face
  AC2 Soft / no-abort: observe-only / zero extra
  AC3 All dual_restore + rehydrate sites under AbortAuthorityHold
  AC4 #3193 ACs preserved; extend persist-rehydrate suite; linter
  AC5 No new mid-struct metric; no docs/design / invent

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

    aud = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    impl = _read("src/compiler/type_checker_impl.cpp")
    ixx = _read("src/compiler/type_checker.ixx")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    qs = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    must("kNestedAbortAuthorityFaceResidualIssue = 3232", "AC1 stamp", aud)
    must("kNestedAbortAuthorityFaceIssue = 3193", "AC1 lineage", aud)
    must("g_abort_authority_in_flight.fetch_add", "AC1 nested count", aud)
    must("compare_exchange_weak", "AC1 last-end", aud)
    must("ac3232_1_nested_hold_keeps_block", "AC1 test", t)

    must("g_abort_authority_hold_observe_total", "AC2 observe", aud)
    must("observe-only", "AC2", aud)
    must("ac3232_2_soft_nested_observe_quiet", "AC2 test", t)

    must("AbortAuthorityHold abort_authority", "AC3 abort sites", mb)
    if mb.count("AbortAuthorityHold abort_authority") < 3:
        fails.append("AC3: expected ≥3 AbortAuthorityHold sites")
    pos = 0
    dual = 0
    while True:
        p = mb.find("abort_restore_dual_topology(", pos)
        if p < 0:
            break
        dual += 1
        hold = mb.rfind("AbortAuthorityHold abort_authority", 0, p)
        if hold < 0:
            fails.append(f"AC3: dual_restore site {dual} missing preceding hold")
        pos = p + 1
    if dual != 3:
        fails.append(f"AC3: expected 3 dual_restore sites, found {dual}")
    must("abort_authority_blocks_rehydrate", "AC3 rehydrate", impl)
    must("Issue #3232", "AC3 rehydrate cite", impl)
    must("Issue #3232", "AC3 densify cite", ixx)
    must("Issue #3232", "AC3 steal cite", steal)
    must("ac3232_3_source_cite_dual_restore_rehydrate", "AC3 test", t)

    must("ac3193_1_prod_hold_blocks_rehydrate", "AC4 3193 preserved", t)
    must("schema-3193", "AC4 3193 schema", qs)
    must("schema-3232", "AC4 schema-3232", qs)
    must("check_nested_abort_authority_face_3232", "AC4 build.py", build)
    must("check_nested_abort_authority_face_3193", "AC4 3193 linter", build)

    if "g_3232_" in aud:
        fails.append("AC5: new g_3232_* counter")
    if (ROOT / "tests" / "issues" / "test_issue_3232.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3232.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3232.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3232.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3232-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3232 nested_abort_authority_face:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3232 nested_abort_authority_face: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
