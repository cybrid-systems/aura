#!/usr/bin/env python3
"""Issue #2988: mutate success drives DefUse / IR / JIT incremental invalidate.

Contract (one row per AC):
  AC1 rebind / query-and-replace success path collects affected NodeIds and
     drives binding_gen + JIT invalidate (apply_closure / eval-current fresh).
  AC2 Production default precise (BFS enqueue); coarse env fallback.
  AC3 query:mutate-invalidate-stats (dirty_nodes, defuse_bumps, jit, binding_gen).
  AC4 Atomic-batch suppress extra bumps (atomic_batch_active).
  AC5 Existing #2038 cascade + #2812 drain retained.
  AC6 Tests extend test_post_mutate_push_cascade; no docs/design / invent.

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    met = _read("src/compiler/observability_metrics.h")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    q = read_query_prims()
    t = _read("tests/compiler/test_post_mutate_push_cascade.cpp")
    build = _read("build.py")

    must("Issue #2988", "AC1", mut)
    must("bump_binding_gen", "AC1", mut)
    must("jit_hotswap_invalidate_total", "AC1", mut)
    must("push_post_mutate_incremental_cascade", "AC1", mut)
    must("ac2988_1_rebind_eval_fresh", "AC1", t)

    must("AURA_MUTATE_INVALIDATE_COARSE", "AC2", mut)
    must("mutate_invalidate_precise_enabled", "AC2", mut)
    must("precise_2988", "AC2", mut)
    must("ac2988_4_atomic_batch_suppress", "AC2", t)

    must("query:mutate-invalidate-stats", "AC3", q)
    must("schema-2988", "AC3", q)
    must("schema-2988", "AC3 relower", obs)
    must("mutate_invalidate_dirty_nodes_total", "AC3", met)
    must("mutate_invalidate_defuse_bumps_total", "AC3", met)
    must("mutate_invalidate_jit_total", "AC3", met)
    must("mutate_invalidate_binding_gen_bumps_total", "AC3", met)
    must("dirty-nodes", "AC3", q)
    must("defuse-bumps", "AC3", q)
    must("jit-invalidate-count", "AC3", q)
    must("binding-gen-bumps", "AC3", q)
    must("ac2988_2_query_stats", "AC3", t)

    must("atomic_batch_active()", "AC4", mut)
    must("suppress_extra_2988", "AC4", mut)

    must("#2038", "AC5", mut)
    must("enqueue_cascade_bfs_invalidate", "AC5", mut)
    must("push_post_mutate_incremental_cascade", "AC5", emb)
    must("#2988", "AC5 ixx", ixx)
    must("schema-2038", "AC5", obs)

    must("ac2988_5_multi_round", "AC6", t)
    must("ac2988_6_source_and_linter", "AC6", t)
    must("check_mutate_invalidate_incremental_2988", "AC6", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2988-*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2988.cpp").is_file():
        fails.append("AC6: test_issue_2988.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "test_edsl_mutate_invalidate_incremental.cpp").is_file():
        fails.append("AC6: invent test_edsl_mutate_invalidate_incremental.cpp")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2988 mutate success invalidate close-loop — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
