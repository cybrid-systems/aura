#!/usr/bin/env python3
"""Issue #2610: auto-detect expected_partial from dirty cone.

Contract:
  AC1 production under-mark + cone → auto_partial + empty-CS hard path
  AC2 explicit expected+has_work matrix unchanged
  AC3 soft observe counter
  AC4 commit_readiness auto_partial reason code 6 + schema-2610
  AC5 test + cmake + build.py; no docs/design

Exit 0 = all rows satisfied.
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
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    dirty = _read("src/compiler/dirty_propagation.ixx")
    test = _read("tests/compiler/test_composite_auto_partial_from_cone_2610.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2610", "AC1", etc)
    must("type_ir_union_cone_nonempty", "AC1", etc + dirty)
    must("composite_commit_auto_partial_from_cone_total", "AC1", etc + aud + met)
    must("auto_partial_from_cone", "AC1", etc)
    must("ac1_auto_partial_hard_miss", "AC1", test)

    # AC2
    must("agent_expected_partial", "AC2", etc)
    must("ac2_explicit_expected_has_work", "AC2", test)

    # AC3
    must("composite_commit_auto_partial_from_cone_observe_total", "AC3", etc + aud)
    must("ac3_soft_observe", "AC3", test)

    # AC4 readiness
    must("auto_partial_from_cone", "AC4", aud)
    must("auto_partial", "AC4", aud)
    must("return 6", "AC4", aud)
    must("schema-2610", "AC4", q + mut)
    must("commit-readiness-force-reason-auto-partial", "AC4", q)
    must("ac4_readiness_and_schema", "AC4", test)

    # AC5
    must("ac5_source_cite", "AC5", test)
    must("test_composite_auto_partial_from_cone_2610", "AC5", cmake)
    must("check_composite_auto_partial_from_cone_2610", "AC5", build)
    must("cmd_composite_auto_partial_from_cone_coverage", "AC5", build)

    # Retain #2509 / #2345
    must("Issue #2509", "retain", etc)
    must("composite_commit_empty_cs_hard_miss_total", "retain", etc)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2610 composite auto-partial from cone — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
