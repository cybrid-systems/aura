#!/usr/bin/env python3
"""Issue #3435: relocate_tracked_objects_for_moving_ must not drop tracking
when try_allocate fails after recycle (UAF bypass).

In ASTArena::relocate_tracked_objects_for_moving_, a small-pool object is
recycled (old slot freed) BEFORE the new slot is obtained. If the
try_allocate / pmr fallback returns null, the old code "continued" without
putting the object back into kept — the live object disappeared from
dtors_ while external pin / slot / canary still held the old address.
That is a UAF / lost-object bypass after the pre-move completeness gates
already passed.

Fix: on !neu, restore the DtorEntry at the old address (kept) and bump
the untracked count so the caller folds into moving_incomplete_remap +
pin_contract_held=false + production sticky-off (#2495 face). No second
remap table, no new GC/pin model, no new query key / mid-struct counter.

Contract:
  AC1  !neu after recycle cannot drop the object from dtors_ — old
       address remains a live tracked object (restore-to-kept at old)
  AC2  failure reuses moving_incomplete_remap + pin_contract_held=false +
       production sticky-off; no new metrics counter / query key
  AC3  success path still only writes last_object_remap_ for addresses
       that moved/rebound; objects_moved stays count of neu != old
  AC4  Soft / Force / Moving-blocked early-return / objects_moved==0
       unchanged (zero extra walk)
  AC5  test injects try_allocate failure after recycle under production
       Moving + required pin; extend test_moving_densify_fail_closed.cpp
       (no test_issue_*.cpp, no docs/design/*)

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

    arena = _read("src/core/arena.ixx")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # AC1: restore-to-old on !neu (no hole in dtors_).
    must("Issue #3435", "AC1 stamp", arena)
    must("if (!neu) {", "AC1 failure branch", arena)
    must("kept.push_back(DtorEntry{p.old, p.dtor, p.size, p.align})", "AC1 restore at old", arena)
    must("ac3435_1_inject_alloc_fail_no_hole", "AC1 test", test)

    # AC2: reuse existing fail-closed face (no new counter / query key).
    must("out_untracked_kept_count", "AC2 untracked bump", arena)
    must("moving_incomplete_remap", "AC2 incomplete face", arena)
    must("pin_contract_held", "AC2 pin-contract face", arena)
    if "g_3435_" in arena and "g_relocate_alloc_fail_inject_remaining" not in arena:
        fails.append("AC2: stray g_3435_* counter (test seam must be the inject flag only)")
    must("ac3435_2_production_hard_sticky", "AC2 test", test)

    # AC3: success path unchanged — remap only for moved/rebound.
    must("last_object_remap_[p.old] = neu", "AC3 remap write", arena)
    must("if (neu != p.old)", "AC3 moved count", arena)
    must("++moved", "AC3 moved bump", arena)
    must("ac3435_3_success_remap_only_moved", "AC3 test", test)

    # AC4: Soft / no-move unchanged.
    must("ac3435_4_soft_zero_extra", "AC4 test", test)

    # AC5: inject seam + test extension + linter wiring + no invent.
    must("g_relocate_alloc_fail_inject_remaining", "AC5 inject seam", arena)
    must("reset_relocate_alloc_fail_inject_for_test", "AC5 reset helper", arena)
    must("ac3435_5_source_and_linter", "AC5 test", test)
    must("check_relocate_drop_tracking_3435", "AC5 build.py", build)
    if (ROOT / "tests" / "core" / "test_issue_3435.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3435.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3435-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    if "schema-3435" in arena or "query:relocate-drop" in arena:
        fails.append("AC5: new query key invented (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3435 relocate drop-tracking — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
