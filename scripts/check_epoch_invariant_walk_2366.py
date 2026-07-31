#!/usr/bin/env python3
"""Issue #2366: per-entry epoch invariant walk + MustDeopt (#2304 follow-up).

  AC1: mode 0 zero-cost early return
  AC2: soft mode + AOT live-behind count / inject
  AC3: IR stamp + closure MustDeopt walk body
  AC4: hard mode abort path
  AC5: query schema-2366 + tests + gate

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

    svc = _read("src/compiler/service.ixx")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    brh = _read("src/compiler/aura_jit_bridge.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_epoch_invariant_walk_2366.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("run_epoch_invariant_if_enabled", "AC1", svc)
    must("aura_epoch_invariant_mode()", "AC1", svc)
    must("mode == 0", "AC1", svc)
    must("ac1_soft_off", "AC1", test)

    must("aura_aot_count_live_generation_behind_slots", "AC2", svc)
    must("aura_aot_inject_live_stale_slot_for_test", "AC2", br)
    must("aura_set_epoch_invariant_mode", "AC2", br)
    must("ac2_soft_detect_stale_aot", "AC2", test)

    must("version_stamp_.bridge_epoch", "AC3", svc)
    must("must_deopt_before_next_call", "AC3", svc)
    must("walk_active_closures", "AC3", svc)
    must("ac3_soft_clean", "AC3", test)

    must("std::abort()", "AC4", svc)
    must("[#2366]", "AC4", svc)
    must("aura_set_epoch_invariant_hard_enabled", "AC4", brh)
    must("ac4_hard_mode", "AC4", test)

    must("schema-2366", "AC5", q)
    must("issue-2366", "AC5", q)
    must("epoch-invariant-wired", "AC5", q)
    must("test_epoch_invariant_walk_2366", "AC5", cmake)
    must("check_epoch_invariant_walk_2366", "AC5", build)
    must("cmd_epoch_invariant_walk_coverage", "AC5", build)
    must("ac5_source_and_query", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2366 epoch invariant per-entry walk — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
