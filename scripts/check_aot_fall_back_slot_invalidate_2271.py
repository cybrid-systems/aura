#!/usr/bin/env python3
"""check_aot_fall_back_slot_invalidate_2271.py — Issue #2271 source gate.

  AC1: C ABI aura_aot_invalidate_all_stale_slots_for_eval declared + helper impl
  AC2: Wired into aura_reload_aot_module_for_eval exhaustion branch
  AC3: Happy-path unchanged (invalidate guarded by fall_back_jit_only)
  AC4: 2 new counters + 4 new query keys + schema-2271/issue-2271 lineage
  AC5: Test extension (tests/compiler/test_aot_reload_primitive.cpp)

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE_H = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
BRIDGE_CPP = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
HOT = ROOT / "src" / "compiler" / "hot_update_registry.hh"
OBS = ROOT / "src" / "compiler" / "observability_metrics.h"
Q = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "compiler" / "test_aot_reload_primitive.cpp"


def main() -> int:
    failures: list[str] = []

    bridge_h = BRIDGE_H.read_text(encoding="utf-8", errors="replace")
    bridge_cpp = BRIDGE_CPP.read_text(encoding="utf-8", errors="replace")
    hot = HOT.read_text(encoding="utf-8", errors="replace")
    obs = OBS.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    # AC1: C ABI declared + helper implemented.
    must("aura_aot_invalidate_all_stale_slots_for_eval", "AC1", bridge_h)
    must(
        'extern "C" std::size_t aura_aot_invalidate_all_stale_slots_for_eval',
        "AC1",
        bridge_cpp,
    )
    must(
        "table_generation.store(0, std::memory_order_release)",
        "AC1",
        bridge_cpp,
    )

    # AC2: wired into exhaustion branch.
    must(
        "aura_aot_invalidate_all_stale_slots_for_eval(eval_ptr)",
        "AC2",
        bridge_cpp,
    )
    must("aura_aot_bump_func_table_epoch();", "AC2", bridge_cpp)
    must("#2271", "AC2", hot)

    # AC3: happy-path unchanged (invalidate guarded by fall_back_jit_only).
    must("if (policy.fall_back_jit_only)", "AC3", bridge_cpp)

    # AC4: counters + query keys + lineage.
    must("aot_reload_fall_back_slot_invalidate_total{0}", "AC4", obs)
    must("aot_reload_fall_back_slot_invalidate_calls_total{0}", "AC4", obs)
    must("aot-reload-fall-back-slot-invalidate-total", "AC4", q)
    must("aot-reload-fall-back-slot-invalidate-calls-total", "AC4", q)
    must("aot-reload-fall-back-slot-invalidate-wired", "AC4", q)
    must("schema-2271", "AC4", q)
    must("issue-2271", "AC4", q)

    # AC5: test extension.
    must("ac2271_physical_invalidate", "AC5", test)
    must("ac2271_physical_invalidate(cs)", "AC5", test)
    must(
        "AC #2271: fall_back_jit_only physical slot invalidate",
        "AC5",
        test,
    )
    must(
        "AC5: aot_reload_fall_back_slot_invalidate_calls_total bumped",
        "AC5",
        test,
    )
    must(
        "AC5: aot_reload_fall_back_slot_invalidate_total bumped",
        "AC5",
        test,
    )
    must("query:aot-stats", "AC5", test)
    must("aura_aot_invalidate_all_stale_slots_for_eval(nullptr)", "AC5", test)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all 5 ACs present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
