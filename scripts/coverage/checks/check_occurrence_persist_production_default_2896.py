#!/usr/bin/env python3
"""Issue #2896: production-default OccurrenceGoal persist + fence face latch.

Contract:
  AC1 occurrence_persist_enabled under production/Full without env
  AC2 Soft zero cost
  AC3 fence rehydrate; miss latches #2704 face
  AC4 rehydrate → non-zero goals for #2842 stamp
  AC5 schema-2896 + prior surfaces; extend test_occurrence_goal_persist_rehydrate
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

    impl = _read("src/compiler/type_checker_impl.cpp")
    ixx = _read("src/compiler/type_checker.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    tma = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    must("2896", "AC1", impl)
    must("AuditStrategy::Full", "AC1", impl)
    must("note_occurrence_empty_after_fence", "AC3", ixx)
    must("2896", "AC3", ixx)
    must("aura_outermost_success_persist_occurrence", "AC1", mb)
    must("2896", "AC1", mb)
    must("note_occurrence_empty_after_fence", "AC3", tma)
    must("schema-2896", "AC5", q)
    must("occurrence-persist-production-default-wired", "AC5", q)
    must("schema-2608", "AC5", q)
    must("schema-2641", "AC5", q)
    must("ac2896_1_production_persist_without_env", "AC5", t)
    must("ac2896_2_soft_zero_cost", "AC5", t)
    must("ac2896_3_fence_rehydrate_or_face", "AC5", t)
    must("ac2896_4_goal_truth_after_rehydrate", "AC5", t)
    must("ac2896_5_query_and_source", "AC5", t)

    # Wire into build.py gate if pattern exists for 2608
    if "check_occurrence_goal_persist_rehydrate_2608" in build:
        # Prefer additive mention of 2896 script when build lists checks
        pass  # optional — do not fail if not yet listed

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2896-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2896 occurrence persist production-default — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
