#!/usr/bin/env python3
"""Issue #4474 (third reopen) source-cite gate: the file-mode reclaim
trigger window must keep scaling on multi-million-object live heaps.

Mechanism (reopen comment 2026-10-18, mutouyuguo):
kReclaimTriggerCeiling=32768 (evaluator.ixx) froze the window above
~1.04M live objects (where live/32 + 128 reaches 32768). Each O(live)
full-heap mark then amortized over at most 32,768 allocations, so the
per-allocation cost grew linearly with the live heap again — the
original #4474 symptom for any program holding a large live data set
(aura-memory: ~3x alloc overhead at 3M live cells vs 300k; write p50
0.82→1.57 ms at a 20k-item store; gdb 25/30 samples in
maybe_reclaim_tw_unreachable → reclaim_tw_unreachable →
file_reclaim_mark_value).

Landed shape (issue direction (a), incremental):
  * The clamp is heap-proportional — reclaim_trigger_ceiling_for =
    max(kReclaimTriggerCeiling, live / kReclaimTriggerLargeHeapDivisor)
    with divisor 4, saturated at uint32. The live/32 scaling itself
    never reaches it (live/4 >= live/32 + 128 for live > ~585), so the
    window keeps growing as live/32 on large heaps and the amortized
    mark cost per allocation stays the ~32 constant.
  * The proportional ceiling only bounds the #4525 burst-doubling
    escalation: small-heap bursts still clamp at the fixed 32768 (the
    #4525 guarantee), a large-heap burst escalates up to live/4.
    Garbage between reclaims grows with the heap instead of the
    amortization breaking — the usual proportional-trigger trade-off.
  * The post-condition contracts moved with the semantics:
    note_live_set_for_reclaim_trigger pins [floor, ceiling_for(live)]
    via the shared helper (the parameter live_objects is in post
    scope); the two accessors pin the floor only, because the upper
    bound is heap-scaled at the writer and readers cannot see the live
    set.

ACs:
  AC1  evaluator.ixx: kReclaimTriggerLargeHeapDivisor (= 4) declared;
       reclaim_trigger_ceiling_for defined next to the constants and
       used by the note_live_set post-condition; NO accessor post
       still pins `r <= kReclaimTriggerCeiling` (the fixed cap must not
       pin the window on large heaps); the constants block cites the
       reopen.
  AC2  evaluator_gc.cpp: note_live_set_for_reclaim_trigger clamps with
       reclaim_trigger_ceiling_for(live_objects) (not the bare fixed
       constant), keeps the #4525 burst-doubling arm (trigger * 2), and
       the in-body contract_assert cites the effective ceiling.
  AC3  Runtime door extends the existing #4474 family test (per
       #81934): tests/compiler/test_pmr_alloc_fiber_safe.cpp defines
       ac4474r3_large_heap_window (3M-live window growth + reopen-repro
       alloc-vs-empty amortization) and registers it in
       run_test_pmr_alloc_fiber_safe; ac4474 AC4 pins the
       proportional-ceiling clamp (huge live set scales past 32768;
       burst escalation saturates at live/4); no
       tests/**/test_issue_4474*.cpp; no docs/design/4474-* (per
       #1655).
  AC4  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
GC = ROOT / "src" / "compiler" / "evaluator_gc.cpp"
FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
TST = ROOT / "tests" / "compiler" / "test_pmr_alloc_fiber_safe.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i : j if j > 0 else len(text)]


def main() -> int:
    ixx = IXX.read_text()
    gc = GC.read_text()
    flat = FLAT.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: heap-proportional ceiling in the decl + contracts ──────
    must("kReclaimTriggerLargeHeapDivisor = 4;" in ixx, "AC1: kReclaimTriggerLargeHeapDivisor declared (= 4)")
    must(
        "reclaim_trigger_ceiling_for("
        in _between(ixx, "kReclaimTriggerLargeHeapDivisor = 4;", "// Issue #4525: when the mark fired"),
        "AC1: reclaim_trigger_ceiling_for defined next to the constants",
    )
    must(
        "std::max<std::uint64_t>(kReclaimTriggerCeiling, proportional)" in ixx,
        "AC1: effective ceiling = max(fixed cap, live/4)",
    )
    must("third reopen" in ixx, "AC1: evaluator.ixx cites the #4474 third reopen")
    post_window = _between(
        ixx, "void note_live_set_for_reclaim_trigger(", "[[nodiscard]] std::uint32_t file_heap_allocs_trigger()"
    )
    must(
        "reclaim_trigger_ceiling_for(live_objects)" in post_window,
        "AC1: note_live_set post pins the heap-scaled ceiling via the shared helper",
    )
    must("r <= kReclaimTriggerCeiling" not in ixx, "AC1: no post/accessor still pins the fixed 32768 cap on the window")
    must(
        "static constexpr std::uint32_t kReclaimTriggerCeiling = 32768;" in ixx,
        "AC1: fixed small-heap cap retained for the #4525 burst guarantee",
    )

    # ── AC2: implementation clamps via the heap-scaled ceiling ──────
    body = _between(gc, "void Evaluator::note_live_set_for_reclaim_trigger(", "bool Evaluator::reclaim_tw_unreachable(")
    must(
        "const auto ceiling = reclaim_trigger_ceiling_for(live_objects);" in body,
        "AC2: clamp reads the heap-scaled ceiling",
    )
    must("std::min<std::uint64_t>(next, ceiling)" in body, "AC2: window clamped against the effective ceiling")
    must("file_heap_allocs_trigger_) * 2)" in body, "AC2: #4525 burst-doubling arm kept")
    must("tw_closure_allocs_trigger_ <= ceiling);" in body, "AC2: contract_assert cites the effective ceiling")
    must("third reopen" in gc, "AC2: evaluator_gc.cpp documents the reopen rationale")
    must("[128, 32768]" not in gc and "[128, 32768]" not in flat, "AC2: no stale [128, 32768] clamp doc left behind")

    # ── AC3: runtime door extends the #4474 family test ─────────────
    must("static void ac4474r3_large_heap_window()" in tst, "AC3: ac4474r3_large_heap_window defined")
    must(
        "ac4474r3_large_heap_window();"
        in _between(tst, "int run_test_pmr_alloc_fiber_safe()", "#ifndef AURA_ISSUE_BATCH_MEMBER"),
        "AC3: ac4474r3_large_heap_window registered in run_test_pmr_alloc_fiber_safe",
    )
    must("scales past the old 32768 ceiling" in tst, "AC3: ac4474 AC4 pins scaling past the old ceiling")
    must("live/4 proportional ceiling" in tst, "AC3: ac4474 AC4 pins the burst clamp at live/4")
    must(
        "measure(3000000)" in tst and "measure(300000)" in tst,
        "AC3: reopen-repro ~3M-cell tier measured against the ~300k flat region",
    )
    must("ovh_big <= 2.0 * ovh_mid + 3.0" in tst, "AC3: alloc amortization bound (2x + slack, pre-fix ~2.9x)")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4474*.cpp"), recursive=True),
        "AC3: no tests/**/test_issue_4474*.cpp (per #81934)",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4474-*")), "AC3: no docs/design/4474-* (per #1655)")

    # ── AC4: wiring ─────────────────────────────────────────────────
    must("check_reclaim_trigger_large_heap_4474.py" in build, "AC4: build.py registers the linter")
    must("check_reclaim_trigger_large_heap_4474.py" in allow, "AC4: root_check_allowlist.txt lists the linter")

    if failures:
        print(f"\ncheck_reclaim_trigger_large_heap_4474: {len(failures)} row(s) failed")
        return 1
    print("\ncheck_reclaim_trigger_large_heap_4474: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
