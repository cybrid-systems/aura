#!/usr/bin/env python3
"""check_envframe_use_site_fence_2268.py — Issue #2268 source gate.

  AC1: EnvFrameRef struct + still_valid / use_site_check / resolve_if_valid
  AC2: materialize_call_env_ref + lookup_by_symid_chain_ref overloads
  AC3: refresh_after_fiber_migration bumps envframe_cache_cleared_on_steal_total
  AC4: 2 new counters + 4 new query keys + schema-2268/issue-2268 lineage
  AC5: test extension (tests/compiler/test_envframe_truncate_epoch.cpp)

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVAL_IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
EVAL_ENV = ROOT / "src" / "compiler" / "evaluator_env.cpp"
EVAL_MUT = ROOT / "src" / "compiler" / "evaluator_fiber_mutation.cpp"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
PRIM_Q = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
TEST = ROOT / "tests" / "compiler" / "test_envframe_truncate_epoch.cpp"


def main() -> int:
    failures: list[str] = []

    eval_ixx = EVAL_IXX.read_text(encoding="utf-8", errors="replace")
    eval_env = EVAL_ENV.read_text(encoding="utf-8", errors="replace")
    eval_mut = EVAL_MUT.read_text(encoding="utf-8", errors="replace")
    metrics = METRICS.read_text(encoding="utf-8", errors="replace")
    prim_q = PRIM_Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    # AC1: EnvFrameRef struct + fields + method decls (in .ixx) + bodies (in .cpp)
    must("struct EnvFrameRef", "AC1", eval_ixx)
    must("EnvId index = NULL_ENV_ID", "AC1", eval_ixx)
    must("std::uint64_t env_gen_stamp = 0", "AC1", eval_ixx)
    must("bool still_valid(Evaluator const& ev) const noexcept", "AC1", eval_ixx)
    must("bool use_site_check(Evaluator const& ev) const noexcept", "AC1", eval_ixx)
    must("resolve_if_valid(Evaluator const& ev) const noexcept", "AC1", eval_ixx)
    must("bool EnvFrameRef::still_valid(Evaluator const& ev) const noexcept", "AC1", eval_env)
    must("bool EnvFrameRef::use_site_check(Evaluator const& ev) const noexcept", "AC1", eval_env)
    must("EnvFrameRef::resolve_if_valid(Evaluator const& ev) const noexcept", "AC1", eval_env)
    must("env_gen_use_site_reject_total.fetch_add(1, std::memory_order_relaxed)", "AC1", eval_env)

    # AC2: Ref-returning overloads
    must("Evaluator::materialize_call_env_ref(const Closure& cl)", "AC2", eval_env)
    must(
        "Evaluator::lookup_by_symid_chain_ref(EnvId start, aura::ast::SymId s) const",
        "AC2",
        eval_env,
    )

    # AC3: refresh_after_fiber_migration bumps envframe_cache_cleared_on_steal_total
    must("envframe_cache_cleared_on_steal_total.fetch_add", "AC3", eval_mut)
    must("fiber->clear_resume_refresh_hints()", "AC3", eval_mut)

    # AC4: counter fields + query keys + schema-2268/issue-2268
    must("env_gen_use_site_reject_total{0}", "AC4", metrics)
    must("envframe_cache_cleared_on_steal_total{0}", "AC4", metrics)
    must("env-gen-use-site-reject-total", "AC4", prim_q)
    must("env-gen-use-site-wired", "AC4", prim_q)
    must("envframe-cache-cleared-on-steal-total", "AC4", prim_q)
    must("envframe-cache-cleared-on-steal-wired", "AC4", prim_q)
    must("schema-2268", "AC4", prim_q)
    must("issue-2268", "AC4", prim_q)

    # AC5: test extension (ac2268_use_site_fence + 5-AC rows)
    must("void ac2268_use_site_fence", "AC5", test)
    must("ac2268_use_site_fence(cs)", "AC5", test)
    must("AC #2268: EnvFrameRef use-site fence", "AC5", test)
    must("AC5: Ref invalidated after env_generation bump", "AC5", test)
    must("AC5: env_gen_use_site_reject_total bumped by use_site_check", "AC5", test)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all 5 ACs present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
