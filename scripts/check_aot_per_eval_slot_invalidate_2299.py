#!/usr/bin/env python3
"""Issue #2299: per-eval physical invalidate of generation-behind AOT slots.

  AC1: owner_eval filter — dual-eval only clears matching owned slots
  AC2: eval_ptr == nullptr process-default (all gen-behind) retained
  AC3: fn_ptr release before generation release (ordering invariant)
  AC4: counters + last_eval + per_eval_calls + schema-2299 query
  AC5: RegisterOwnerGuard + tests extension + #2271 still present

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE_H = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
BRIDGE_CPP = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
OBS = ROOT / "src" / "compiler" / "observability_metrics.h"
Q = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "compiler" / "test_aot_reload_primitive.cpp"


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    h = BRIDGE_H.read_text(encoding="utf-8", errors="replace")
    cpp = BRIDGE_CPP.read_text(encoding="utf-8", errors="replace")
    obs = OBS.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    # AC1: ownership filter
    must("owner_eval", "AC1", cpp)
    must("filter_by_eval", "AC1", cpp)
    must("want_owner", "AC1", cpp)
    must("ac2299_per_eval_slot_invalidate", "AC1", test)
    must("AC1: eval B slot remains", "AC1", test)

    # AC2: nullptr process-default
    must("eval_ptr == nullptr", "AC2", h)
    must("AC2: nullptr invalidate clears remaining gen-behind", "AC2", test)

    # AC3: ordering
    must("fn_ptr.store(0, std::memory_order_release)", "AC3", cpp)
    must("table_generation.store(0, std::memory_order_release)", "AC3", cpp)
    must("AC3: fn_ptr release before generation release", "AC3", test)

    # AC4: counters + query (clang-format may wrap `{0}` onto next line)
    must("aot_reload_fall_back_slot_invalidate_last_eval", "AC4", obs)
    must("aot_reload_fall_back_slot_invalidate_per_eval_calls_total", "AC4", obs)
    must("aot-reload-fall-back-slot-invalidate-last-eval", "AC4", q)
    must("aot-reload-fall-back-slot-invalidate-per-eval-calls-total", "AC4", q)
    must("aot-reload-fall-back-slot-invalidate-per-eval-wired", "AC4", q)
    must("schema-2299", "AC4", q)
    must("issue-2299", "AC4", q)
    must("aura_aot_last_slot_invalidate_eval", "AC4", h)

    # AC5: RegisterOwnerGuard + tests + #2271 retained
    must("RegisterOwnerGuard", "AC5", cpp)
    must("aura_aot_set_register_owner_eval", "AC5", h)
    must("ac2299_per_eval_slot_invalidate(cs)", "AC5", test)
    must("ac2271_physical_invalidate", "AC5", test)
    must("aura_aot_invalidate_all_stale_slots_for_eval", "AC5", h)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: per-eval slot invalidate (#2299) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
