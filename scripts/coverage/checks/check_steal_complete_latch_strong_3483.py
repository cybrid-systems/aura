#!/usr/bin/env python3
"""Issue #3483: call_steal_complete must refuse weak no-op when latch==1.

call_steal_complete is off the success enqueue path (transaction owns it)
but remains a live helper. ELF weak fiber_bridge makes
`if (aura_evaluator_on_steal_complete)` true without the strong evaluator
TU, so the helper never reached the production abort arm. Gate on the
existing multi-worker latch + strong marker.

Contract:
  AC1  latch==1 → strong-only; abort + reuse fail_total if weak
  AC2  transaction Ok path unchanged (no call_steal_complete(stolen))
  AC3  unlatched Soft/light: existing pointer path + missing-entry total
  AC4  no new residual counter / query key
  AC5  extend test_steal_complete_gc_defer with runtime strong vs weak
  AC6  this linter AFTER #2844; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    wc = _read("src/serve/worker.cpp")
    ss = _read("src/serve/steal_safety.cpp")
    fb = _read("src/compiler/fiber_bridge.cpp")
    t = _read("tests/serve/test_steal_complete_gc_defer.cpp")
    build = _read("build.py")

    fn = wc.find("static inline void call_steal_complete")
    win = wc[fn : fn + 2200] if fn >= 0 else ""
    must("Issue #3483", "AC1 helper cite", win)
    must("g_production_multi_worker_latched", "AC1 latch", win)
    must("aura_abi_strong_steal_complete_v()", "AC1 strong marker", win)
    must("g_production_abi_selfcheck_fail_total", "AC4 reuse fail_total", win)
    must("std::abort()", "AC1 abort if weak", win)
    must("aura_evaluator_on_steal_complete(stolen)", "AC1 strong call", win)

    must_not("call_steal_complete(stolen)", "AC2 no worker success call", wc)
    must("aura_evaluator_on_steal_complete(stolen)", "AC2 transaction still owns complete", ss)
    must("try_begin_steal_decision", "AC2 per-Fiber decision window", ss)

    must("bump_steal_complete_entry_missing_total", "AC3 light missing-entry", win)
    must("if (aura_evaluator_on_steal_complete)", "AC3 unlatched pointer path", win)
    must("steal_snapshot_soft_production_locked", "AC3 #2377 null abort retained", win)

    must("aura_abi_strong_steal_complete_v", "AC5 weak marker source", fb)
    must("3483 AC5: this binary links strong steal-complete", "AC5 runtime", t)
    must("aura_test_call_steal_complete", "AC5 test seam", t)
    must("ac3483_1_runtime_latch_strong_only", "AC5 AC1 runtime", t)

    must("check_steal_complete_latch_strong_3483", "AC6 build.py", build)
    prev = build.find("check_steal_sole_enqueue_gate_2844")
    ours = build.find("check_steal_complete_latch_strong_3483")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #2844")
    must_not("schema-3483", "AC4 no schema-3483", wc)
    must_not("g_3483_", "AC4 no g_3483_*", wc)
    if _read("tests/serve/test_issue_3483.cpp") or _read("tests/issues/test_issue_3483.cpp"):
        fails.append("AC6: test_issue_3483.cpp present (forbidden #81967)")
    if _read("docs/design/3483-steal-complete-latch.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3483 steal_complete_latch_strong:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3483 steal_complete_latch_strong: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
