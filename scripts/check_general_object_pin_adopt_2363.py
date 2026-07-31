#!/usr/bin/env python3
"""Issue #2363: complete GeneralObjectPin adoption coverage.

  AC1: wire_general_object_create_pair + note wire helper
  AC2: two-pin pattern (pool+flat) — no single-pin overwrite
  AC3: all 7 inventory sites wired (mutate/batch/require/query/guard/load/eval-expr)
  AC4: Soft zero cost retained + Moving remap/verify retained
  AC5: query schema-2363 + tests + gate

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

    lp = _read("src/core/lifetime_pin.ixx")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    qw = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    ev = _read("src/compiler/evaluator_primitives_eval.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_general_object_pin_adopt_2363.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 helper
    must("wire_general_object_create_pair", "AC1", lp)
    must("note_general_object_pin_mutate_wire", "AC1", lp)
    must("kGeneralObjectPinAdoptIssue = 2363", "AC1", lp)
    must("kGeneralObjectPinAdoptSiteCount = 7", "AC1", lp)
    must("ac1_wire_pair", "AC1", test)

    # AC2 two-pin (no overwrite of pool by flat on one pin)
    must("pat_pool_pin", "AC2", mut)
    must("pat_flat_pin", "AC2", mut)
    must("wire_general_object_create_pair", "AC2", mut)
    # Old single-pin double-call must be gone
    if "pat_pin.pin(static_cast<void*>(pat_pool)" in mut:
        fails.append("AC2: mutate still uses single pat_pin for both buffers")

    # AC3 seven sites
    must("wire_general_object_create_pair", "AC3", mut)
    must("site 2/7", "AC3", flat)
    must("site 3/7", "AC3", flat)
    must("site 4/7", "AC3", qw)
    must("site 5/7", "AC3", qw)
    must("site 6/7", "AC3", ev)
    must("site 7/7", "AC3", ev)
    must("ac5_inventory_query", "AC3", test)

    # AC4 Soft + Moving machinery
    must("Soft/Force do not relocate", "AC4", lp)
    must("verify_pins_under_moving_compact", "AC4", lp)
    must("ac4_soft_zero", "AC4", test)

    # AC5 query + gate (production surface is memory.cpp — wins over obs_eval)
    mem = _read("src/compiler/evaluator_primitives_memory.cpp")
    must("schema-2363", "AC5", mem)
    must("issue-2363", "AC5", mem)
    must("general-object-pin-adopt-complete-wired", "AC5", mem)
    must("general-object-pin-adopt-site-count", "AC5", mem)
    must("kGeneralObjectPinAdoptSiteCount", "AC5", mem)
    must("schema-2363", "AC5", q)  # also kept on obs_eval for dual path
    must("test_general_object_pin_adopt_2363", "AC5", cmake)
    must("check_general_object_pin_adopt_2363", "AC5", build)
    must("cmd_general_object_pin_adopt_coverage", "AC5", build)
    must("ac2_pin_moving_validate", "AC5", test)
    must("ac3_unpinned_fail", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2363 complete GeneralObjectPin adopt — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
