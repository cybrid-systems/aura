#!/usr/bin/env python3
"""Issue #2353: post-densify / post-steal Linear+Type revalidate coverage.

  AC1: Ordered phase after Moving densify / steal mismatch
  AC2: Fail-closed via report type_ok / overall_ok + fail counter
  AC3: Soft / no densify / no linear → early return (zero new atomics)
  AC4: Observability counters + schema-2353 on lifetime-contract-snapshot
  AC5: Tests + source-cite Phase 5 / steal / enforce

Exit 0 = all ACs satisfied.
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

    dcr = _read("src/core/densify_consistency_report.h")
    arena = _read("src/core/arena.ixx")
    env = _read("src/compiler/evaluator_env.cpp")
    eixx = _read("src/compiler/evaluator.ixx")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_post_densify_linear_type_revalidate_2353.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 ordered phase
    must("run_post_densify_linear_type_revalidate", "AC1", eixx)
    must("run_post_densify_linear_type_revalidate", "AC1", env)
    must("linear_post_mutate_enforce_all", "AC1", env)
    must("had_moving_densify", "AC1", emb)
    must("run_post_densify_linear_type_revalidate", "AC1", emb)
    must("moved_live_objects", "AC1", arena)
    must("Issue #2353", "AC1", emb)
    must("ac1_densify_with_linear_runs", "AC1", test)

    # AC2 fail-closed
    must("type_ok", "AC2", dcr)
    must("type_ok", "AC2", emb)
    must("post_densify_linear_type_fail_total", "AC2", met)
    must("post_densify_linear_type_fail_total", "AC2", env)
    must("ac2_fail_closed_report", "AC2", test)
    must("overall_ok", "AC2", dcr)

    # AC3 zero cost
    must("!had_moving_densify", "AC3", env)
    must("ac3_soft_no_densify_zero_cost", "AC3", test)
    must("AC3: Soft", "AC3", env)

    # AC4 observability
    must("post_densify_linear_type_revalidate_total", "AC4", met)
    must("post_densify_linear_type_revalidate_total", "AC4", fields)
    must("post_densify_linear_type_fail_total", "AC4", fields)
    must("schema-2353", "AC4", q)
    must("issue-2353", "AC4", q)
    must("densify-type-ok", "AC4", q)
    must("post-densify-linear-type-revalidate-total", "AC4", q)
    must("post-densify-linear-type-wired", "AC4", q)
    must("ac4_query_schema", "AC4", test)

    # AC5 steal + tests + gate
    must("run_post_densify_linear_type_revalidate", "AC5", fm)
    must("Issue #2353", "AC5", fm)
    must("ac5_source_cite", "AC5", test)
    must("test_post_densify_linear_type_revalidate_2353", "AC5", cmake)
    must("check_post_densify_linear_type_revalidate_2353", "AC5", build)
    must("cmd_post_densify_linear_type_revalidate_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2353 post-densify Linear+Type revalidate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
