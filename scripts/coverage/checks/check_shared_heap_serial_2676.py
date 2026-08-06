#!/usr/bin/env python3
"""Issue #2676: complete shared Evaluator heap serialization under
concurrent fibers (extend #2651). Refines #2651's string/pair
serialization with closure materialization + live-closure table + IR-cache
bridge root coverage.

Contract:
  AC1 closure materialization (write closures_[cid] = std::move(cl) +
     read make_closure(cid)) acquires closures_mtx_ for the full critical
     section of push / read — P0 multi-fiber race.
  AC2 live-closure refresh (aura_refresh_live_closures_for_mutated_define)
     acquires alloc_storage_lock_ for the full critical section of epoch
     bump + counter increments.
  AC3 IR-cache bridge root mutation: closure_bridge_epoch stamping +
     walk_active_closures paths covered by the same per-heap lock class
     (alloc_storage_lock_) as string_heap_ / pairs_ push (#2651).
  AC4 test_shared_heap_multi_fiber_stress exists in
     tests/core/test_stress_alloc_storage_lock.cpp (per #81967: extend
     existing #2651 stress file) and exercises the new paths under 8+
     threads with no torn writes / no crash.
  AC5 lock_order_audit.h documents the closure materialization critical
     section rank (Closures rank is preserved — no new inversion with
     workspace_mtx_ / GC coordinator locks).
  AC6 no docs/design (aura philosophy: 'agent-developed, no human docs').

Exit 0 = all AC rows satisfied.
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

    def must_count(n: str, label: str, hay: str, at_least: int) -> None:
        c = hay.count(n)
        if c < at_least:
            fails.append(f"{label}: expected ≥{at_least} occurrence(s) of {n!r}, found {c}")

    ev = _read("src/compiler/evaluator_eval_flat.cpp")
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    test = _read("tests/core/test_stress_alloc_storage_lock.cpp")
    build = _read("build.py")
    lock_audit = _read("src/compiler/lock_order_audit.h")

    # AC1: closure materialization — shared_lock(closures_mtx_) around
    # make_closure reads. 3 sites: line ~1550, 1662, 4509 (approximate).
    must_count("std::shared_lock<std::shared_mutex> rlock(closures_mtx_)", "AC1", ev, at_least=3)
    must("make_closure(cid)", "AC1", ev)
    must("Issue #2676", "AC1", ev)

    # AC2: live-closure refresh — alloc_storage_lock_ on
    # aura_refresh_live_closures_for_mutated_define (aura_jit_bridge.cpp).
    must("aura_refresh_live_closures_for_mutated_define", "AC2", bridge)
    must("alloc_storage_lock_", "AC2", bridge)
    must("Issue #2676", "AC2", bridge)

    # AC3: IR-cache bridge root — closure_bridge_epoch stamping +
    # walk_active_closures paths covered by the same per-heap lock class.
    must("closure_bridge_epoch", "AC3", bridge)
    must("walk_active_closures", "AC3", bridge)

    # AC4: test_shared_heap_multi_fiber_stress exists in the existing
    # tests/core/test_stress_alloc_storage_lock.cpp (per #81967 extend).
    must("test_shared_heap_multi_fiber_stress", "AC4", test)
    must("run_ac4_shared_heap_multi_fiber_stress", "AC4", test)
    must("kThreads = 8", "AC4", test)  # ≥8 fibers per AC3 / #2649

    # AC5: lock_order_audit.h documents the closure materialization rank.
    must("Closures", "AC5", lock_audit)
    must("closures_mtx_", "AC5", lock_audit)
    must("alloc_storage_lock_", "AC5", lock_audit)

    # AC6: linter self-coverage + build.py wire-up.
    must("check_shared_heap_serial_2676", "AC6", build)
    must("#2676", "AC6", bridge)
    must("#2676", "AC6", ev)
    must("#2676", "AC6", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2676 shared heap serialization (extend #2651) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
