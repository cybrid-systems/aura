#!/usr/bin/env python3
"""Issue #2640: production Restricted default periodic epoch-invariant soft walk
(physically clear generation-behind AOT slots + MustDeopt stale live closures
on a steady-clock interval under production Soft mode).

  AC1: periodic walk hook + counters in bridge + production gating
  AC2: env-driven period (AURA_EPOCH_INVARIANT_PERIOD_MS, default 5000)
  AC3: dtor wire-up at MutationBoundaryGuard outermost success exit
  AC4: query surface (schema-2640 + 7 additive component counters + wired flag)
  AC5: test extension in tests/compiler/test_epoch_invariant_walk_2366.cpp
       (ac2640_* functions + Issue #2640 ACs)
  AC6: gate wired into build.py (cmd_epoch_invariant_periodic_coverage)

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

    br = _read("src/compiler/aura_jit_bridge.cpp")
    brh = _read("src/compiler/aura_jit_bridge.h")
    brs = _read("src/compiler/aura_jit_bridge_stub.cpp")
    dtor = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_epoch_invariant_walk_2366.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — bridge hook + counters
    must("Issue #2640", "AC1", br)
    must("aura_periodic_epoch_invariant_walk_if_due", "AC1", br)
    must("aura_epoch_invariant_periodic_walks_total_v_read", "AC1", br)
    must("aura_epoch_invariant_periodic_last_walk_at_ms_v_read", "AC1", br)
    must("aura_epoch_invariant_periodic_skipped_off_total_v_read", "AC1", br)
    must(
        "aura_epoch_invariant_periodic_skipped_wrong_mode_total_v_read",
        "AC1",
        br,
    )
    must(
        "aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read",
        "AC1",
        br,
    )
    must(
        "aura_epoch_invariant_periodic_skipped_disabled_total_v_read",
        "AC1",
        br,
    )
    must("aura_set_epoch_invariant_periodic_period_ms", "AC1", br)
    must("production_defaults_active", "AC1", br)  # gating
    must("aura_epoch_invariant_must_deopt_stale_live_closures", "AC1", br)  # reuse
    must("aura_aot_invalidate_all_stale_slots_for_eval", "AC1", br)  # reuse
    must("aura_periodic_epoch_invariant_walk_if_due", "AC1", brh)  # decl

    # AC2 — env-driven period
    must("AURA_EPOCH_INVARIANT_PERIOD_MS", "AC2", br)
    must("g_epoch_invariant_periodic_period_ms", "AC2", br)

    # AC3 — dtor wire-up
    must("aura_periodic_epoch_invariant_walk_if_due", "AC3", dtor)
    must("Issue #2640", "AC3", dtor)
    must("outermost && success", "AC3", dtor)

    # AC3 — stub TU has weak stub so light bundles link clean
    must("aura_periodic_epoch_invariant_walk_if_due", "AC3", brs)
    must("aura_epoch_invariant_periodic_walks_total_v_read", "AC3", brs)

    # AC4 — query surface (schema additive — schema-2366 must remain)
    must("schema-2640", "AC4", q)
    must("issue-2640", "AC4", q)
    must("epoch-invariant-periodic-wired", "AC4", q)
    must("component-epoch-invariant-periodic-walks-total", "AC4", q)
    must("component-epoch-invariant-periodic-last-walk-at-ms", "AC4", q)
    must("component-epoch-invariant-periodic-period-ms", "AC4", q)
    must("component-epoch-invariant-periodic-skipped-off-total", "AC4", q)
    must(
        "component-epoch-invariant-periodic-skipped-wrong-mode-total",
        "AC4",
        q,
    )
    must(
        "component-epoch-invariant-periodic-skipped-rate-limited-total",
        "AC4",
        q,
    )
    must(
        "component-epoch-invariant-periodic-skipped-disabled-total",
        "AC4",
        q,
    )
    # Prior schemas retained (Issue #2366 lineage).
    must("schema-2366", "AC4", q)
    must("schema-2506", "AC4", q)

    # AC5 — test extension (extend existing file per #81967)
    must("ac2640_periodic_walk_clears_stale", "AC5", test)
    must("ac2640_off_mode_skips_walk", "AC5", test)
    must("ac2640_2541_semantics_preserved", "AC5", test)
    must("ac2640_rate_limit_amortizes", "AC5", test)
    must("ac2640_counters_and_query", "AC5", test)
    must("ac2640_source_and_linter", "AC5", test)
    must("apply_production_audit_defaults", "AC5", test)
    must("aura_set_epoch_invariant_periodic_period_ms", "AC5", test)
    must("aura_epoch_invariant_periodic_walks_total_v_read", "AC5", test)
    must(
        "aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read",
        "AC5",
        test,
    )
    must("test_epoch_invariant_walk_2366", "AC5", cmake)
    must("aura_issue_test_link_llvm_jit(test_epoch_invariant_walk_2366)", "AC5", cmake)

    # AC6 — gate wired into build.py
    must("cmd_epoch_invariant_periodic_coverage", "AC6", build)
    must("check_epoch_invariant_periodic_coverage.py", "AC6", build)
    must("cmd_epoch_invariant_periodic_coverage()", "AC6", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2640 production Restricted default periodic epoch-invariant soft walk — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
