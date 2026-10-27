#!/usr/bin/env python3
"""Issue #4608 source-cite gate: the file-mode reclaim must reclaim the
string heap, not only pairs, cells and closures.

Mechanism (the issue):
A string value IS an index into string_heap_. The file-mode reclaim
(#4459 / #4474) recycled pairs, cells and closures, but never the
string heap: every string minted and dropped inside a running program
kept its slot until exit. The cost is about 70 bytes per string (the
slot object, the #4428 owner record and the vector's growth slack), so
a loop that mints and drops strings (number->string, a c-func that
returns a String) grew the heap — and the RSS — linearly:
57.3 MB at 100k iterations vs 120.2 MB at 1M on the issue's build.

Landed shape (issue direction (a), a free list):
  * The string heap owns a free list. The reclaim marks the string
    slots with the same mark it uses for pairs (a string value is the
    slot index) and releases the unreachable slots; the next mint
    reuses a slot instead of growing the heap. The heap's high-water
    mark is then the number of CONCURRENTLY live strings, not the
    number of strings ever minted.
  * A reused slot is a fresh content write: the #4548 code-point
    cache entry for that slot is dropped (a stale entry would be
    trusted when the new content has the same byte length), and the
    #4428 owner stamp is rewritten by the mint hook, so a reused slot
    never carries the previous owner's tenant.
  * A string mint counts as a file-heap allocation (it drives the
    same reclaim window as a pair/vector mint). Without that a loop
    that only mints strings never reached the reclaim trigger.
  * The IR interpreter keeps NO index-parallel copy of the string
    heap: a copy would go stale when the reclaim releases a slot and
    the next mint re-uses it. The IR reads the content from the
    evaluator heap (the only string store).
  * The caches that hold string indices (the evaluator's intern
    caches, the IR's const string/keyword caches) are reclaim roots,
    so a cached index is never released under the cache.

ACs:
  AC1  locked_string_heap.hh: the free list lives in the string-heap
       owner (free_slots_ / slot_free_ / release_unmarked /
       free_slot_snapshot); mint() reuses a released slot; a reused
       slot drops its cached code-point index (invalidate_cp_index_locked)
       and re-runs the mint hook (the #4428 owner stamp); the
       structural writes (resize / clear / replace / unsafe_storage)
       keep the free list consistent with the heap.
  AC2  evaluator_gc.cpp: the file-mode mark marks the string slots
       (seen_string) and BOTH reclaim paths (file-mode + tree-walker)
       release the unreachable slots (release_unreachable_string_slots);
       the intern caches are roots; the live string slots count toward
       the #4474 reclaim trigger.
  AC3  evaluator.ixx: a string mint counts toward the reclaim window
       (note_inrange_string_mint), the release resets the #4428 owner
       record, and the live/free slot accessors exist for the runtime
       door.
  AC4  ir_executor.ixx / ir_executor_impl.cpp: the IR keeps no
       index-parallel string heap copy and reads the content from the
       evaluator heap; the const string/keyword caches are reclaim
       roots.
  AC5  Runtime door: tests/compiler/test_pmr_alloc_fiber_safe.cpp
       defines ac4608_string_heap_reclaim (flat heap across 20k
       dropped strings + the survival program for env/pair/vector/
       closure/cell) and registers it in run_test_pmr_alloc_fiber_safe;
       no tests/**/test_issue_4608*.cpp; no docs/design/4608-* (per #1655).
  AC6  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEAP = ROOT / "src" / "compiler" / "locked_string_heap.hh"
GC = ROOT / "src" / "compiler" / "evaluator_gc.cpp"
IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
IR_IXX = ROOT / "src" / "compiler" / "ir_executor.ixx"
IR_IMPL = ROOT / "src" / "compiler" / "ir_executor_impl.cpp"
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
    heap = HEAP.read_text()
    gc = GC.read_text()
    ixx = IXX.read_text()
    ir_ixx = IR_IXX.read_text()
    ir_impl = IR_IMPL.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: the free list lives in the string-heap owner ───────────
    must("std::vector<std::size_t> free_slots_;" in heap, "AC1: the heap owns the free list")
    must("std::vector<std::uint8_t> slot_free_;" in heap, "AC1: the free-slot bitmap is index-parallel")
    must("release_unmarked(" in heap, "AC1: the heap exposes the release (release_unmarked)")
    must("free_slot_snapshot()" in heap, "AC1: the heap exposes the mark-time free snapshot")
    mint = _between(heap, "std::uint64_t mint(Args&&... args)", "// ── reads (vector surface)")
    must("if (!free_slots_.empty())" in mint, "AC1: mint() reuses a released slot")
    must("v_[idx].swap(tmp);" in mint, "AC1: the reuse drops the old content (and its buffer)")
    must("invalidate_cp_index_locked(idx);" in mint, "AC1: the reuse drops the cached code-point index")
    must("hook_(hook_ctx_, idx);" in mint, "AC1: the reuse re-runs the mint hook (the #4428 stamp)")
    must(
        "invalidate_cp_index_locked(size_type idx) noexcept" in heap, "AC1: the per-slot code-point invalidation exists"
    )
    for op, needle in (
        ("resize", "slot_free_.resize(n, 0);"),
        ("clear", "free_slots_.clear();"),
        ("replace", "free_slots_.clear();"),
        ("unsafe_storage", "free_slots_.clear();"),
    ):
        body = _between(
            heap,
            f"void {op}(" if op != "unsafe_storage" else "Vec& unsafe_storage()",
            "static constexpr std::size_t npos",
        )
        must(needle in body, f"AC1: {op} keeps the free list consistent with the heap")

    # ── AC2: the mark + the release in both reclaim paths ───────────
    mark = _between(gc, "static void file_reclaim_mark_value(", "bool Evaluator::reclaim_file_mode_unreachable(")
    must("seen_string" in mark, "AC2: the file-mode mark marks the string slots")
    must("if (is_string(v))" in mark, "AC2: the mark handles a string value (the slot index)")
    file_mode = _between(
        gc, "bool Evaluator::reclaim_file_mode_unreachable(", "bool Evaluator::reclaim_tw_unreachable("
    )
    tw = _between(gc, "bool Evaluator::reclaim_tw_unreachable(", "void Evaluator::abandon_file_heap_freelists(")
    for name, body in (("file-mode", file_mode), ("tree-walker", tw)):
        must(
            "release_unreachable_string_slots(seen_string, free_at_mark)" in body,
            f"AC2: the {name} reclaim releases the unreachable string slots",
        )
        must("seen_string(n_strings, 0)" in body, f"AC2: the {name} reclaim sizes the string mark")
        must("free_slot_snapshot()" in body, f"AC2: the {name} reclaim snapshots the free slots")
        must("short_str_cache_" in body, f"AC2: the {name} reclaim roots the intern caches")
        must("live_strings" in body, f"AC2: the {name} reclaim counts the live string slots")
    must(
        "note_live_set_for_reclaim_trigger(live_pairs + live_vecs + live_strings" in file_mode,
        "AC2: the file-mode trigger counts the live string slots",
    )
    must("live_cells + live_strings" in tw, "AC2: the tree-walker trigger counts the live string slots")

    # ── AC3: the evaluator side ────────────────────────────────────
    mint_hook = _between(
        ixx,
        "void note_inrange_string_mint(std::size_t idx) noexcept",
        "// Issue #4543: test read of the #4428 owner record",
    )
    must(
        "file_heap_allocs_since_reclaim_.fetch_add(1, std::memory_order_relaxed);" in mint_hook,
        "AC3: a string mint counts toward the reclaim window",
    )
    must("release_unreachable_string_slots(" in ixx, "AC3: the evaluator exposes the release")
    must("string_slot_tenants_[i] = 0;" in ixx, "AC3: the release resets the #4428 owner record")
    must("string_heap_live_size()" in ixx, "AC3: the live-slot accessor exists (runtime door)")
    must("string_heap_free_slots()" in ixx, "AC3: the free-slot accessor exists (runtime door)")

    # ── AC4: the IR keeps no index-parallel copy ───────────────────
    must("std::vector<std::string> string_heap_;" not in ir_ixx, "AC4: the IR keeps no index-parallel string heap copy")
    must("Issue #4608" in ir_ixx, "AC4: ir_executor.ixx cites the #4608 rationale")
    must(
        "const LockedStringHeap& string_heap" in ir_impl, "AC4: the IR reads the string content from the evaluator heap"
    )
    must(
        "string_heap_.size()" not in ir_impl and "string_heap_[idx]" not in ir_impl,
        "AC4: the IR never consults a local mirror",
    )
    must(
        "const_string_cache_" in ir_impl and "const_keyword_cache_" in ir_impl,
        "AC4: the IR const string caches are reclaim roots",
    )

    # ── AC5: the runtime door ───────────────────────────────────────
    must("static void ac4608_string_heap_reclaim()" in tst, "AC5: ac4608_string_heap_reclaim defined")
    must(
        "ac4608_string_heap_reclaim();"
        in _between(tst, "int run_test_pmr_alloc_fiber_safe()", "#ifndef AURA_ISSUE_BATCH_MEMBER"),
        "AC5: ac4608_string_heap_reclaim registered in run_test_pmr_alloc_fiber_safe",
    )
    must("the string heap stays flat across 20k dropped strings" in tst, "AC5: the flat-heap door is asserted")
    must("live strings survive the reclaim (env/pair/vector/closure/cell)" in tst, "AC5: the survival door is asserted")
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4608*.cpp"), recursive=True),
        "AC5: no tests/**/test_issue_4608*.cpp (per #81934)",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4608-*")), "AC5: no docs/design/4608-* (per #1655)")

    # ── AC6: wiring ─────────────────────────────────────────────────
    must("check_string_heap_reclaim_4608.py" in build, "AC6: build.py registers the linter")
    must("check_string_heap_reclaim_4608.py" in allow, "AC6: root_check_allowlist.txt lists the linter")

    if failures:
        print(f"\ncheck_string_heap_reclaim_4608: {len(failures)} row(s) failed")
        return 1
    print("\ncheck_string_heap_reclaim_4608: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
