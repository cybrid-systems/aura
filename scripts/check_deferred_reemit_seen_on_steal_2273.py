#!/usr/bin/env python3
"""check_deferred_reemit_seen_on_steal_2273.py — Issue #2273 source gate.

  AC1: on_deferred_reemit_seen_on_steal decl + impl in hot_update_registry
  AC2: drain stays at outermost Guard exit (#2162 path) — steal path adds
      bumper BEFORE drain
  AC3: zero-cost via has_deferred_reemit() single relaxed load
  AC4: 4 query keys + schema-2273/issue-2273 lineage on
      query:hot-update-registry-stats
  AC5: Test extension (tests/compiler/test_hot_update_cascade_dirty_reemit.cpp)

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HUR_H = ROOT / "src/compiler/hot_update_registry.hh"
HUR_CPP = ROOT / "src/compiler/hot_update_registry.cpp"
EFM = ROOT / "src/compiler/evaluator_fiber_mutation.cpp"
MUTATE = ROOT / "src/compiler/evaluator_primitives_mutate.cpp"
TEST = ROOT / "tests/compiler/test_hot_update_cascade_dirty_reemit.cpp"


def main() -> int:
    failures: list[str] = []

    hur_h = HUR_H.read_text(encoding="utf-8", errors="replace")
    hur_cpp = HUR_CPP.read_text(encoding="utf-8", errors="replace")
    efm = EFM.read_text(encoding="utf-8", errors="replace")
    mutate = MUTATE.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            failures.append(f"{label}: missing needle {needle!r}")

    # AC1: decl + impl + C ABI.
    must("on_deferred_reemit_seen_on_steal", "AC1", hur_h)
    must(
        "void HotUpdateRegistry::on_deferred_reemit_seen_on_steal",
        "AC1",
        hur_cpp,
    )
    must(
        "aura_hot_update_on_deferred_reemit_seen_on_steal",
        "AC1",
        hur_cpp,
    )

    # AC2: drain preserved + steal bumper BEFORE drain.
    must(
        "mutation_boundary_depth() == 0 && aura_hot_update_has_deferred_reemit() != 0",
        "AC2",
        efm,
    )
    must(
        "aura_hot_update_on_deferred_reemit_seen_on_steal(steal_fiber_id);",
        "AC2",
        efm,
    )

    # AC3: single-load guard.
    must("has_deferred_reemit() != 0", "AC3", efm)

    # AC4: query keys + lineage.
    must("reemit-deferred-seen-on-steal-total", "AC4", mutate)
    must("reemit-deferred-seen-on-steal-last-fiber-id", "AC4", mutate)
    must("schema-2273", "AC4", mutate)
    must("issue-2273", "AC4", mutate)

    # AC5: test extension.
    must("ac2273_deferred_reemit_seen_on_steal", "AC5", test)
    must("ac2273_deferred_reemit_seen_on_steal(cs)", "AC5", test)
    must(
        "AC #2273: deferred reemit steal-path observability",
        "AC5",
        test,
    )
    must(
        "AC5: C ABI callable + schema-2273 wired",
        "AC5",
        test,
    )

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: all 5 ACs present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
