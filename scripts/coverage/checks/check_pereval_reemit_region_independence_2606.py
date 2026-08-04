#!/usr/bin/env python3
"""Issue #2606: PerEval / multi-AotState reemit + invalidate independence.

Contract:
  AC1 reemit candidate loop filters by eval owner; skip metric bumped
  AC2 invalidate_for_eval owner filter retained (#2299 parity)
  AC3 nullptr / single-eval soft path (no filter when owner unset)
  AC4 schema-2606 + reemit_cross_eval_candidate_skipped_total query axes
  AC5 test + cmake + build.py gate + process-global epoch docs

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    bc = _read("src/compiler/aura_jit_bridge.cpp")
    bh = _read("src/compiler/aura_jit_bridge.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    met = _read("src/compiler/observability_metrics.h")
    qq = _read("src/compiler/evaluator_primitives_query.cpp")
    qe = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    dirty = _read("src/compiler/service_dirty.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/compiler/test_pereval_reemit_region_independence.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    q_surface = qq + qe

    # AC1 reemit filter + metric
    must("Issue #2606", "AC1", bc)
    must("reemit_cross_eval_candidate_skipped_total", "AC1", bc)
    must("reemit_cross_eval_candidate_skipped_total", "AC1", met)
    must("aura_aot_set_reemit_owner_eval", "AC1", bh)
    must("aura_aot_get_reemit_owner_eval", "AC1", bh)
    must("aura_aot_set_reemit_owner_eval", "AC1", bc)
    must("g_aot_reemit_owner_eval", "AC1", bc)
    must("filter_by_eval", "AC1", bc)
    must("aura_aot_set_reemit_owner_eval", "AC1", stub)

    # AC2 invalidate parity retained
    must("aura_aot_invalidate_all_stale_slots_for_eval", "AC2", bc + bh)
    must("owner_eval", "AC2", bc)

    # AC3 soft path / process-default
    must("process-default", "AC3", bc + bh)
    must("nullptr", "AC3", bh)

    # AC4 query (production surface is obs_eval; query.cpp also carries keys)
    must("schema-2606", "AC4", q_surface)
    must("issue-2606", "AC4", q_surface)
    must("reemit_cross_eval_candidate_skipped_total", "AC4", q_surface)
    must("reemit-cross-eval-filter-wired", "AC4", q_surface)
    must("schema-2606", "AC4-prod", qe)
    must("reemit_cross_eval_candidate_skipped_total", "AC4-prod", qe)

    # Host stamps
    must("aura_aot_set_reemit_owner_eval", "AC1-host", dirty)
    must("aura_aot_set_reemit_owner_eval", "AC1-host", mb)

    # AC5 wiring
    must("ac1_dual_eval_reemit_owner_filter", "AC5", test)
    must("ac2_invalidate_for_eval_isolation", "AC5", test)
    must("ac3_nullptr_path_no_filter", "AC5", test)
    must("ac4_query_and_single_eval_zero", "AC5", test)
    must("test_pereval_reemit_region_independence", "AC5", cmake)
    must("check_pereval_reemit_region_independence_2606", "AC5", build)
    must("cmd_pereval_reemit_region_independence_coverage", "AC5", build)
    must("process-global", "AC5", bh + bc)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2606 PerEval reemit region independence — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
