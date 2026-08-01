#!/usr/bin/env python3
"""Issue #2461: per-If stable narrowing cache key (shape × epoch × refined).

Contract:
  AC1 shape hash + hit path
  AC2 structural key fields in PredicateMemoEntry
  AC3 goal note + selective invalidate
  AC4 epoch gate
  AC5 schema-2461 + #2359 lineage + gate wiring

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

    tci = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    test = _read("tests/compiler/test_occurrence_cache_key_2461.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("Issue #2461", "AC1", tci)
    must("cond_shape_hash", "AC1", tci)
    must("cond_shape_hash", "AC1", impl)
    must("hash_node_shape", "AC1", impl)
    must("occurrence_cache_key_hits_", "AC1", tci)
    must("AC1: same pred shape", "AC1", test)

    must("refined", "AC2", tci)
    must("invalidate_predicate_memo_for_var_names", "AC2", tci)
    must("AC2: structural key isolates", "AC2", test)

    must("note_occurrence_goal", "AC3", impl)
    must("AC3: goal note on miss path", "AC3", test)

    must("epoch == cache_epoch_", "AC4", impl)
    must("AC4: epoch gate in resolve", "AC4", test)

    must("schema-2461", "AC5", q)
    must("occurrence-cache-key-hit-total", "AC5", q)
    must("occurrence-cache-key-miss-total", "AC5", q)
    must("occurrence_cache_key_hit_total", "AC5", met)
    must("occurrence_cache_key_hit_total", "AC5", fields)
    must("schema-2359", "AC5", q)
    must("test_occurrence_cache_key_2461", "gate", cmake)
    must("check_occurrence_cache_key_2461", "gate", build)
    must("cmd_occurrence_cache_key_coverage", "gate", build)
    must("AC5: schema-2461", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: occurrence cache key #2461 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
