#!/usr/bin/env python3
"""Issue #3810: Soft live_compact gen-bump uses this-window freelist, not lifetime.

Soft/Force live_compact previously treated
  relocated = free_slot_count() + recycle_hits()
as freelist activity for gen bump + pin invalidation. recycle_hits_ is a
lifetime counter (never reset). After the first freelist recycle ever,
every Soft had relocated > 0 → perpetual gen bump + wipe-all LifetimePins.

#3810 drives Soft gen bump from this-window freelist activity (delta recycle
hits vs baseline / holes closed this Soft call / saved_bytes) and keeps
lifetime recycle_hits_ as metrics only. Soft stays non-Moving.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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
            fails.append(f"{label}: unexpected {n!r}")

    ixx = _read("src/core/arena.ixx")
    test = _read("tests/serve/test_gc_compact_sweep_batch.cpp")
    build = _read("build.py")
    gf = _read("scripts/coverage/simple_check_grandfather.txt")

    rel = ixx.find("// ── Relocate (freelist protocol) ──")
    gen = ixx.find(
        "if (saved_bytes > 0 || this_window_relocated > 0 || result.moved_live_objects)",
        rel if rel != -1 else 0,
    )
    win = ixx[rel:gen] if rel != -1 and gen != -1 and gen > rel else ""

    # AC1/AC3: this-window predicate, not lifetime recycle_hits in relocated
    must("Issue #3810", "AC3 cite Relocate", win)
    must("recycle_hits_at_entry", "AC3 Soft-entry snapshot", win)
    must("live_compact_recycle_hits_baseline_", "AC3 baseline delta", win)
    must("holes_closed", "AC1 holes_closed this call", win)
    must("this_window_relocated", "AC1 this_window_relocated", win)
    must_not(
        "const std::size_t reuses = small_pool_.recycle_hits();",
        "AC3 no lifetime reuses",
        win,
    )
    must_not(
        "const std::size_t relocated = holes + reuses;",
        "AC3 no holes+lifetime relocated",
        win,
    )
    must(
        "if (saved_bytes > 0 || this_window_relocated > 0 || result.moved_live_objects)",
        "AC1/AC2 gen-bump predicate",
        ixx[gen : gen + 120] if gen != -1 else "",
    )
    must(
        "live_compact_recycle_hits_baseline_ = 0;",
        "AC3 baseline member",
        ixx,
    )
    # Lifetime metrics accessor retained
    must(
        "[[nodiscard]] std::size_t recycle_hits() const noexcept { return recycle_hits_; }",
        "AC3 metrics accessor retained",
        ixx,
    )

    # Soft stays non-Moving (remap still Moving-only)
    must("pass is Moving-only", "AC Soft non-Moving remap", ixx)

    # Test harness
    must("run_3810_soft_gen_bump_this_window", "AC4 test harness", test)
    must("3810 AC4", "AC4 soak marker", test)
    must("3810 AC1", "AC1 quiet Soft marker", test)
    must("3810 AC2", "AC2 this-window bump marker", test)

    # Wire
    must("check_soft_gen_bump_this_window_3810.py", "build wire", build)
    must("check_soft_gen_bump_this_window_3810.py", "grandfather", gf)

    if fails:
        print("FAIL: Issue #3810 Soft gen-bump this-window linter")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3810 Soft gen-bump this-window")
    return 0


if __name__ == "__main__":
    sys.exit(main())
