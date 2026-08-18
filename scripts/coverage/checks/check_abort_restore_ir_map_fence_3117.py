#!/usr/bin/env python3
"""Issue #3117: dual-topology abort restore IR cache + source_to_ir_map fence.

Publish abort_force_generation before restore, zero-restamp + clear
source_to_ir_map under the same fence, refuse lazy rebuild from
pre-abort IR. Soft/clean windows stay zero extra cost. No new query keys.

Contract:
  AC1  begin fence before abort_restore_dual_topology (3 dual-topology arms)
  AC2  force-dirty clears map + abort_map_invalid; ensure refuses refill
  AC3  store_define_v2 clears abort_map_invalid; Soft/clean unchanged
  AC4  extend test_mutation_rollback_coverage; linter; no test_issue_3117;
       no docs/design/; no new query keys

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    svc = _read("src/compiler/service.ixx")
    ev = _read("src/compiler/evaluator.ixx")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    pure = _read("src/compiler/ir_cache_pure.ixx")
    t = _read("tests/compiler/test_mutation_rollback_coverage.cpp")
    build = _read("build.py")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    must("begin_abort_ir_cache_force_fence", "AC1 begin", svc)
    must("set_abort_ir_cache_begin_force_fn", "AC1 setter", ev)
    if bound.count("abort_ir_cache_begin_force_fn_") < 3:
        fails.append("AC1: expected ≥3 begin-force call sites before dual-topology restore")
    if bound.count("abort_restore_dual_topology") < 3:
        fails.append("AC1: expected ≥3 abort_restore_dual_topology sites")
    must("3117 AC1", "AC1 test", t)

    must("abort_map_invalid", "AC2 flag", svc)
    must("source_to_ir_map.clear()", "AC2 clear map", svc)
    must("if (entry.abort_map_invalid)", "AC2 ensure refuse", svc)
    must("Issue #3117", "AC2 pure cite", pure)
    must("3117 AC2", "AC2 test", t)

    must("entry.abort_map_invalid = false", "AC3 store clears", svc)
    must("3117 AC3", "AC3 store test", t)

    must("3117", "AC4 extend suite", t)
    must("check_abort_restore_ir_map_fence_3117", "AC4 build.py", build)
    if "schema-3117" in q:
        fails.append("AC4: new query key schema-3117 (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3117.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3117.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3117-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3117 abort restore IR map fence — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
