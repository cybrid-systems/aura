#!/usr/bin/env python3
"""Issue #3781: densify rewrite uses this-window relocate pairs only.

#3469 keeps previous-window keys in last_object_remap_ so
resolve_object_remap(A) still hits after A→B→C (apply_closure densify-stale
refuse). Fold skips only this-window live_new addresses. A recycled
small-pool address A holding a new unmoved object Y is not in live_new, so
tombstone A→B is restored. Pre-#3781 rewrite paths (slot / pin / linear /
RootRemap) walked the full map and silently rewrote Y's slot/pin to B when
some other object moved — silent UAF, not fail-closed (#3633 cover uses
last_moving_relocated_old_ and misses the wrong rewrite).

Fix: rewrite / mutation consumers use this_window_remap built from
last_moving_relocated_old_; last_object_remap_ remains resolve/refuse-only.

Contract:
  AC1 rewrite paths use this_window_remap / last_moving_relocated_old_
  AC2 resolve_object_remap still hits full #3469 table (fold retained)
  AC3 #3633 cover still uses last_moving_relocated_old_ denominator
  AC4 Soft/Off / no-move: this_window_remap empty → zero rewrite work
  AC5 no second pin registry; no test_issue_3781.cpp; no docs/design/3781-*
  AC6 tests extend test_moving_densify_fail_closed / multi-window 3469

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

    arena = _read("src/core/arena.ixx")
    moving = _read("tests/core/test_moving_compact.cpp")
    fail_closed = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    must("Issue #3781", "AC1 cite", arena)
    must("this_window_remap", "AC1 this-window map", arena)
    must("this_window_remap.find(*slot)", "AC1 slot rewrite", arena)
    must("for (const auto& [old_ptr, new_ptr] : this_window_remap)", "AC1 pin remap", arena)
    must("remap_linear_roots_under_moving(\n                    this_window_remap", "AC1 linear", arena)
    must("invoke_root_remap_callback_(result, &root_remap_covered_old, this_window_remap)", "AC1 RootRemap", arena)
    must("count_post_moving_stale_known_ptrs_(this_window_remap)", "AC1 stale scan", arena)
    must(
        "for (void* old_ptr : last_moving_relocated_old_)",
        "AC1 build from relocated-old",
        arena,
    )

    # Rewrite must NOT fold the full last_object_remap_ into pin/slot walks.
    pin_block = arena.find("Issue #3781: pin remap walks this-window pairs only")
    if pin_block < 0:
        fails.append("AC1: pin remap #3781 cite missing")
    else:
        pin_win = arena[pin_block : pin_block + 900]
        if "for (const auto& [old_ptr, new_ptr] : last_object_remap_)" in pin_win:
            fails.append("AC1: pin remap still iterates full last_object_remap_")

    must("Issue #3469", "AC2 fold retained", arena)
    must("prev_remap", "AC2 prev_remap", arena)
    must("resolve_object_remap", "AC2 resolve API", arena)
    must(
        "auto it = last_object_remap_.find(old_ptr);\n        return it == last_object_remap_.end()",
        "AC2 resolve still full table",
        arena,
    )

    must(
        "if (result.objects_moved > 0 && !last_moving_relocated_old_.empty()) {",
        "AC3 #3633 denominator",
        arena,
    )
    must("last_moving_relocated_old_.push_back(p.old)", "AC3 relocated-old fill", arena)

    must("this_window_remap.clear()", "AC4 clear", arena)
    must(
        "result.objects_moved > 0 && !last_moving_relocated_old_.empty()",
        "AC4 populate gated",
        arena,
    )
    must("result.moved_live_objects && !this_window_remap.empty()", "AC4 rewrite gate", arena)

    must_not("class DensifyClosurePinRegistry", "AC5 no second registry", arena)
    must_not("g_3781_", "AC5 no invented counter", arena)
    must_not("schema-3781", "AC5 no new query key", arena)
    if (ROOT / "tests" / "core" / "test_issue_3781.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3781.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3781.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3781.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3781-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    must("3781", "AC6 fail_closed suite", fail_closed)
    must("ac3781_", "AC6 fail_closed tests", fail_closed)
    must("3469: A still a remap key after window 2", "AC6 3469 retained", moving)
    must("check_densify_this_window_rewrite_3781", "AC6 build.py", build)

    if fails:
        print("FAIL #3781 densify_this_window_rewrite:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3781 densify_this_window_rewrite: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
