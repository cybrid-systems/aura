#!/usr/bin/env python3
"""check_pin_owner_state_machine_2270.py — Issue #2270 source gate.

  AC1: PinOwner enum + mark_ffi_owned / release_ffi / owner() / helpers
  AC2: PresentGuard RAII in render_primitives.cpp
  AC3: render_pin_blocked_moving_total counter + blocks_arena_reclaim()
  AC4: 4 counters + 6 query keys + schema-2270/issue-2270 lineage
  AC5: Test extension (tests/core/test_gc_defer_render_pin_2160.cpp)

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LP = ROOT / "src" / "core" / "lifetime_pin.ixx"
REN = ROOT / "src" / "renderer" / "render_primitives.cpp"
OBS = ROOT / "src" / "compiler" / "observability_metrics.h"
Q = ROOT / "src" / "compiler" / "evaluator_primitives_io.cpp"
TEST = ROOT / "tests" / "core" / "test_gc_defer_render_pin_2160.cpp"


def main() -> int:
    failures: list[str] = []

    lp = LP.read_text(encoding="utf-8", errors="replace")
    ren = REN.read_text(encoding="utf-8", errors="replace")
    obs = OBS.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    # AC1: enum + new methods + move semantics.
    must("enum class PinOwner", "AC1", lp)
    must("FfiBorrowed", "AC1", lp)
    must("FfiOwned", "AC1", lp)
    must("void mark_ffi_owned()", "AC1", lp)
    must("void release_ffi()", "AC1", lp)
    must("PinOwner owner()", "AC1", lp)
    must("ffi_holds_ownership()", "AC1", lp)
    must("blocks_arena_reclaim()", "AC1", lp)
    must("owner_(o.owner_)", "AC1", lp)
    must("o.owner_ = PinOwner::None", "AC1", lp)

    # AC2: PresentGuard RAII.
    must("struct PresentGuard", "AC2", ren)
    must("pin.mark_ffi_handoff()", "AC2", ren)
    must("pin.release_ffi()", "AC2", ren)

    # AC3: Moving block counter + helper.
    must("render_pin_blocked_moving_total{0}", "AC3", obs)
    must("blocks_arena_reclaim()", "AC3", lp)

    # AC4: counters + query keys + lineage.
    must("pin_owner_arena_total{0}", "AC4", obs)
    must("pin_owner_ffi_borrowed_total{0}", "AC4", obs)
    must("pin_owner_ffi_owned_total{0}", "AC4", obs)
    must("pin-owner-arena-transitions", "AC4", q)
    must("pin-owner-ffi-borrowed-transitions", "AC4", q)
    must("pin-owner-ffi-owned-transitions", "AC4", q)
    must("render-pin-blocked-moving-total", "AC4", q)
    must("pin-owner-state-machine-wired", "AC4", q)
    must("schema-2270", "AC4", q)
    must("issue-2270", "AC4", q)

    # AC5: test extension.
    must("void ac2270_pin_owner_state", "AC5", test)
    must("ac2270_pin_owner_state(cs)", "AC5", test)
    must("AC #2270: PinOwner state machine", "AC5", test)
    must(
        "AC5: mark_ffi_owned() \u2192 FfiOwned",
        "AC5",
        test,
    )
    must(
        "AC5: blocks_arena_reclaim() == true under FfiOwned",
        "AC5",
        test,
    )
    must("AC5: move ctor transfers owner_ (FfiOwned)", "AC5", test)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all 5 ACs present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
