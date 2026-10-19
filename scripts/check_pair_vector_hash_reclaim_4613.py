#!/usr/bin/env python3
"""Issue #4613 source-cite gate: the file-mode reclaim must reclaim the
pairs minted by vector->list, the vectors minted by list->vector and the
(hash) tables.

Mechanism (the issue):
The file-mode reclaim (#4459 / #4474) recycled pairs, cells, closures and
(after #4608) strings, but three primitives minted their objects OUTSIDE
the evaluator mint path:

  * vector->list pushed the pairs straight into pairs_ (a raw
    pairs.push_back), so the reclaim never saw them as reclaimable and
    the mint never counted toward the reclaim window. A 64-slot vector
    leaked 64 pairs per call - about 3.2 KB per iteration.
  * list->vector pushed the result vector straight into vector_heap_ (a
    raw vector_heap.push_back), so the result's element buffer was never
    freed - about 0.55 KB per iteration.
  * (hash) published a fresh FlatHashTable into the process-global
    g_hash_tables and nothing ever freed the block - about 0.57 KB per
    iteration, and a per-request (hash) leaked for the process lifetime.

Landed shape:
  * vector->list mints its pairs through mint_pair (the free-list path
    the list constructors use) and holds the per-heap ctor lock while
    the list is half-built, so a reclaim cannot free the fresh pairs.
  * list->vector mints its result through mint_vector (the free-list
    path make-vector / vector use).
  * (hash) publishes the table through mint_hash_table: the evaluator
    owns a hash free list (slot index + released bitmap + the
    per-evaluator ownership bitmap), a released slot is reused instead
    of growing g_hash_tables, and the mint charges the reclaim window
    by the table's real byte size (without a charge a hash-only loop
    never reached the trigger, and with a one-unit charge the window
    held 32768 live tables - 18 MB - between two marks).
  * The reclaim marks the hash tables it walks (seen_hash), snapshots
    the slot table pointers under the mark's own hash lock and releases
    the tables this evaluator minted that the mark proved unreachable:
    the block is freed, the owner stamp is reset (#4428) and the slot
    goes on the free list. A table the mark did not see (another
    evaluator, the JIT, a query primitive) is never released.

ACs:
  AC1  evaluator_primitives_vector.cpp: vector->list mints the pairs
       through mint_pair (and holds the ctor lock), list->vector mints
       the vector through mint_vector, (hash) publishes the table
       through mint_hash_table; the raw push_back shapes are gone.
  AC2  evaluator_gc.cpp: the file-mode mark marks the hash slots
       (seen_hash); BOTH reclaim paths (file-mode + tree-walker)
       snapshot the slot tables under the mark's hash lock and release
       the unreachable tables; the live hash tables count toward the
       #4474 reclaim trigger.
  AC3  evaluator.ixx: the hash free list (hash_reuse_freelist_ /
       hash_on_freelist_ / hash_owned_), the mint (reuse + reclaim
       window + owner stamp), the release (free the block, reset the
       owner stamp, free the slot) and the runtime doors.
  AC4  Runtime door: tests/compiler/test_pmr_alloc_fiber_safe.cpp
       defines ac4613_pair_vector_hash_reclaim (flat pair / vector /
       hash heaps across 20k dropped objects + the survival program for
       a live vector, a live list and a live hash) and registers it in
       run_test_pmr_alloc_fiber_safe; no tests/**/test_issue_4613*.cpp;
       no docs/design/4613-* (per #1655).
  AC5  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRIMS = ROOT / "src" / "compiler" / "evaluator_primitives_vector.cpp"
GC = ROOT / "src" / "compiler" / "evaluator_gc.cpp"
IXX = ROOT / "src" / "compiler" / "evaluator.ixx"
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


def _squeeze(text: str) -> str:
    """Drop whitespace and quote characters.

    clang-format may split a long expression or a long string literal over
    several lines, so a source-cite row must not depend on the exact line
    breaks. Squeezing keeps the token sequence and drops only layout.
    """
    return re.sub(r'[\s"]+', "", text)


def has(text: str, needle: str) -> bool:
    return _squeeze(needle) in _squeeze(text)


def main() -> int:
    prims = PRIMS.read_text()
    gc = GC.read_text()
    ixx = IXX.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: the three primitives mint through the evaluator heap ────
    v2l = _between(prims, '"vector->list",', '"(vector) -> list"')
    must(bool(v2l), "AC1: the vector->list primitive body found")
    must(
        has(v2l, "ev.mint_pair(*it, result, /*check_quota=*/true)"),
        "AC1: vector->list mints its pairs through mint_pair (the free-list path)",
    )
    must(
        "pairs.push_back(" not in v2l,
        "AC1: vector->list no longer pushes the pairs straight into pairs_",
    )
    must(
        "Evaluator::ListCtorLockHold hold(ev);" in v2l,
        "AC1: vector->list holds the per-heap ctor lock while the list is half-built",
    )
    must(
        has(v2l, "ev.bump_pair_alloc_count_n("),
        "AC1: vector->list counts the minted pairs",
    )

    l2v = _between(prims, '"list->vector",', '"(list) -> vector"')
    must(bool(l2v), "AC1: the list->vector primitive body found")
    must(
        has(l2v, "ev.mint_vector(std::move(elems), /*check_quota=*/true)"),
        "AC1: list->vector mints its result through mint_vector (the free-list path)",
    )
    must(
        "vector_heap.push_back(" not in l2v,
        "AC1: list->vector no longer pushes the result straight into vector_heap_",
    )
    must(
        "Evaluator::ListCtorLockHold hold(ev);" in l2v,
        "AC1: list->vector holds the per-heap lock across the pair walk",
    )

    hash_prim = _between(prims, '"hash",', '"(...kvs) -> hash"')
    must(bool(hash_prim), "AC1: the (hash) primitive body found")
    must(
        has(hash_prim, "ev.mint_hash_table(ht)"),
        "AC1: (hash) publishes the table through the evaluator's hash free list",
    )

    # ── AC2: the mark + the release in both reclaim paths ────────────
    mark = _between(gc, "static void file_reclaim_mark_value(", "bool Evaluator::reclaim_file_mode_unreachable(")
    must("seen_hash" in mark, "AC2: the file-mode mark marks the hash slots")
    must("if (is_hash(v))" in mark, "AC2: the mark handles a hash value (the slot index)")

    file_mode = _between(
        gc, "bool Evaluator::reclaim_file_mode_unreachable(", "bool Evaluator::reclaim_tw_unreachable("
    )
    tw = _between(gc, "bool Evaluator::reclaim_tw_unreachable(", "void Evaluator::abandon_file_heap_freelists(")
    for name, body in (("file-mode", file_mode), ("tree-walker", tw)):
        must(
            "release_unreachable_hash_tables(seen_hash, hash_at_mark)" in body,
            f"AC2: the {name} reclaim releases the unreachable hash tables",
        )
        must(
            "hash_at_mark.assign(g_hash_tables.begin(), g_hash_tables.end());" in body,
            f"AC2: the {name} reclaim snapshots the slot tables under the mark's hash lock",
        )
        must(
            "live_hashes" in body,
            f"AC2: the {name} reclaim counts the live hash tables",
        )
    must(
        has(file_mode, "note_live_set_for_reclaim_trigger(live_pairs + live_vecs + live_strings + live_hashes"),
        "AC2: the file-mode trigger counts the live hash tables",
    )
    must(
        has(tw, "live_cells + live_strings + live_hashes"),
        "AC2: the tree-walker trigger counts the live hash tables",
    )

    # ── AC3: the evaluator side (the hash free list) ─────────────────
    must(
        "std::vector<std::size_t> hash_reuse_freelist_;" in ixx,
        "AC3: the evaluator owns the hash free list",
    )
    must(
        "std::vector<std::uint8_t> hash_on_freelist_;" in ixx,
        "AC3: the released-slot bitmap is index-parallel",
    )
    must(
        "std::vector<std::uint8_t> hash_owned_;" in ixx,
        "AC3: the per-evaluator ownership bitmap is index-parallel",
    )
    mint = _between(ixx, "std::size_t mint_hash_table(FlatHashTable* ht) {", "release_unreachable_hash_tables(")
    must(bool(mint), "AC3: the mint_hash_table body found")
    must(
        "while (!hash_reuse_freelist_.empty())" in mint,
        "AC3: the publish reuses a released slot",
    )
    must(
        "g_hash_tables[id] = ht;" in mint,
        "AC3: the publish takes the released slot",
    )
    must(
        "hash_on_freelist_[id] = 0;" in mint,
        "AC3: the reused slot leaves the released bitmap",
    )
    must(
        "g_hash_tables.push_back(ht);" in mint,
        "AC3: the publish still grows g_hash_tables when no slot was released",
    )
    must(
        "hash_owned_[hidx] = 1;" in mint,
        "AC3: the published table is this evaluator's (the release only frees those)",
    )
    must(
        "g_hash_tenants[hidx] = capability_tenant_id_;" in mint,
        "AC3: the publish stamps the executing evaluator on the table",
    )
    must(
        "kReclaimWindowUnitBytes" in mint,
        "AC3: a hash mint charges the reclaim window by its memory footprint",
    )
    must(
        "FlatHashTable::total_bytes(ht->capacity)" in mint,
        "AC3: the window charge is the table's real byte size",
    )
    release = _between(ixx, "std::size_t release_unreachable_hash_tables(", "hash_table_free_slots()")
    must(bool(release), "AC3: the release body found")
    must(
        "FlatHashTable::destroy(ht);" in release,
        "AC3: the release frees the unreachable table's block",
    )
    must(
        "g_hash_tables[i] = nullptr;" in release,
        "AC3: the release clears the released slot",
    )
    must(
        "g_hash_tenants[i] = 0;" in release,
        "AC3: the release resets the owner stamp (#4428)",
    )
    must(
        "hash_reuse_freelist_.push_back(i);" in release,
        "AC3: the release puts the slot on the free list",
    )
    must(
        "hash_owned_[i]" in release,
        "AC3: the release only touches a table this evaluator minted",
    )
    must(
        "g_hash_tables[i] != at_mark[i]" in release,
        "AC3: a table re-minted since the mark is left alone",
    )
    must(
        "++hash_tables_released_;" in release,
        "AC3: the release counts the released tables (the runtime door)",
    )
    for door in (
        "hash_table_free_slots()",
        "hash_table_slots()",
        "hash_tables_released_total()",
        "hash_tables_minted_total()",
        "pair_heap_free_slots()",
        "vector_heap_size()",
        "vector_heap_free_slots()",
    ):
        must(door in ixx, f"AC3: the runtime door {door} exists")

    # ── AC4: the runtime door ────────────────────────────────────────
    must(
        "static void ac4613_pair_vector_hash_reclaim()" in tst,
        "AC4: ac4613_pair_vector_hash_reclaim defined",
    )
    must(
        "ac4613_pair_vector_hash_reclaim();"
        in _between(tst, "int run_test_pmr_alloc_fiber_safe()", "#ifndef AURA_ISSUE_BATCH_MEMBER"),
        "AC4: ac4613_pair_vector_hash_reclaim registered in run_test_pmr_alloc_fiber_safe",
    )
    must(
        "the pair heap stays flat across 20k vector->list calls" in tst,
        "AC4: the flat pair heap door is asserted",
    )
    must(
        "the vector heap stays flat across 20k list->vector calls" in tst,
        "AC4: the flat vector heap door is asserted",
    )
    must(
        "the hash table count stays flat across 20k (hash) calls" in tst,
        "AC4: the flat hash table count door is asserted",
    )
    must(
        "the reclaim released the dead hash tables" in tst,
        "AC4: the hash release door is asserted",
    )
    must(
        "the live vector / list / hash survive the reclaim (3 of 3)" in tst,
        "AC4: the survival door is asserted",
    )
    must(
        not glob.glob(str(ROOT / "tests" / "**" / "test_issue_4613*.cpp"), recursive=True),
        "AC4: no tests/**/test_issue_4613*.cpp (per #81934)",
    )
    must(not glob.glob(str(ROOT / "docs" / "design" / "4613-*")), "AC4: no docs/design/4613-* (per #1655)")

    # ── AC5: wiring ──────────────────────────────────────────────────
    must(
        "check_pair_vector_hash_reclaim_4613.py" in build,
        "AC5: build.py registers the linter",
    )
    must(
        "check_pair_vector_hash_reclaim_4613.py" in allow,
        "AC5: root_check_allowlist.txt lists the linter",
    )

    if failures:
        print(f"\ncheck_pair_vector_hash_reclaim_4613: {len(failures)} row(s) failed")
        return 1
    print("\ncheck_pair_vector_hash_reclaim_4613: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
