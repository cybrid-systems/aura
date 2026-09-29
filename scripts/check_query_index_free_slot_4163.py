#!/usr/bin/env python3
"""Issue #4163 source-cite gate: tag_arity_index free-slot hygiene.

free_orphan_nodes_from (rollback / parse-failure cleanup, #1299/#1300)
zeroes node_gen_ only — parent_ survives as a stale non-null leftover.
The tag_arity_index stale-entry prune only dropped out-of-range ids and
parent_==NULL orphans (#484 arm), and insert_node's orphan check was
likewise parent-based — so a free-slot tombstone (node_gen_==0) with a
leftover parent_ stayed in (or re-entered) (tag,arity) buckets. After an
outermost mutate, the query:pattern index fast path could serve a
recycled NodeId into the production schema-2 QueryResult (later mutate
resolve fails stale-ref — poisoned multi-round Agent memory; the #4162
family's full-scan walks were already guarded, the index layer was not).

Fix shape: reuse the public #1299/#1300 is_free_slot predicate (no
second model):
  - tag_arity_index_insert_node skips free slots before the #484
    parent-based orphan check (covers rebuild_full / append_nodes /
    sync_after_mutation re-insert).
  - tag_arity_index_prune_stale_entries is_stale treats free slots as
    stale regardless of parent_ (clears tag_arity_indexed_key_).
  - query:pattern fast-path serve loop skips free slots (warm bucket +
    quiet sync path never re-prunes — serve guard is the last line of
    defense before a recycled id reaches the QueryResult).

ACs:
  AC1  tag_arity_index_insert_node guards flat.is_free_slot(id) before
       the #484 parent-based orphan check, cites #4163.
  AC2  tag_arity_index_prune_stale_entries is_stale has the free-slot
       arm (clears tag_arity_indexed_key_, returns true), cites #4163.
  AC3  query:pattern fast-path serve loop guards is_free_slot after the
       range check and before the macro-introduced check, cites #4163.
  AC4  test home + registry: test_query_index_composite.cpp hosts the
       #4163 ACs and dispatches them from run_test_query_index_composite;
       no tests/compiler/test_issue_4163.cpp; no docs/design/4163-*;
       build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists
       check_query_index_free_slot_4163.py.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QIDX = ROOT / "src" / "compiler" / "evaluator_query_index.cpp"
QWS = ROOT / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp"
TST = ROOT / "tests" / "compiler" / "test_query_index_composite.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

checks: list[tuple[bool, str]] = []


def must(cond: bool, label: str) -> None:
    checks.append((bool(cond), label))


def _fn_window(text: str, start_anchor: str, end_anchor: str) -> str:
    """Slice from a function signature to the next sibling signature."""
    i = text.find(start_anchor)
    if i < 0:
        return ""
    j = text.find(end_anchor, i + len(start_anchor))
    return text[i:j] if j > 0 else text[i:]


def main() -> int:
    qidx = QIDX.read_text() if QIDX.exists() else ""
    qws = QWS.read_text() if QWS.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOW.read_text() if ALLOW.exists() else ""

    must(bool(qidx), "evaluator_query_index TU readable")
    must(bool(qws), "query_workspace TU readable")

    # ── AC1: insert_node free-slot guard ──
    w = _fn_window(
        qidx,
        "void Evaluator::tag_arity_index_insert_node(",
        "void Evaluator::tag_arity_index_remove_node(",
    )
    must(bool(w), "AC1: insert_node window found")
    must("Issue #4163: skip free-list tombstones" in w, "AC1: insert_node cites #4163")
    g = w.find("if (flat.is_free_slot(id))")
    orphan = w.find("Issue #484: skip orphan nodes")
    must(g >= 0, "AC1: insert_node guards is_free_slot")
    must(orphan >= 0 and (g < orphan), "AC1: guard precedes the #484 parent-based orphan check")
    must("return;" in w[g : g + 80], "AC1: guard is fail-closed (early return)")

    # ── AC2: prune is_stale free-slot arm ──
    w = _fn_window(
        qidx,
        "void Evaluator::tag_arity_index_prune_stale_entries(",
        "void Evaluator::tag_arity_index_sync_after_mutation(",
    )
    must(bool(w), "AC2: prune window found")
    must("Issue #4163: free-list tombstones" in w, "AC2: prune cites #4163")
    g = w.find("if (flat.is_free_slot(id))")
    parent = w.find("flat.parent_of(id) == aura::ast::NULL_NODE")
    must(g >= 0, "AC2: is_stale has free-slot arm")
    must(parent >= 0 and (g < parent), "AC2: free-slot arm precedes the #484 parent arm")
    arm = w[g : g + 300] if g >= 0 else ""
    must("kTagArityKeyNone" in arm and "return true;" in arm, "AC2: free-slot arm clears indexed_key and is stale")

    # ── AC3: pattern fast-path serve guard ──
    w = _prim_window_qws(qws)
    must(bool(w), "AC3: pattern fast-path window found")
    must("Issue #4163: skip free-list tombstones" in w, "AC3: serve loop cites #4163")
    g = w.find("if (flat.is_free_slot(id))")
    rng = w.find("if (id >= flat.size())")
    mac = w.find("flat.is_macro_introduced(id)")
    must(g >= 0, "AC3: serve loop guards is_free_slot")
    must(rng >= 0 and mac >= 0 and (rng < g < mac), "AC3: guard sits between range check and macro-introduced check")

    # Contrast anchors pinned by the issue body: the #484 checks survive.
    must(qidx.find("Issue #484: skip orphan nodes") >= 0, "contrast: #484 insert orphan check intact")
    must(
        qidx.find("Issue #1501: keep user-only hygiene index in sync.") >= 0, "contrast: #1501 user-index prune intact"
    )

    # ── AC4: test home + registry ──
    must(bool(tst), "test_query_index_composite TU readable")
    must(tst.find("--- #4163 AC1:") >= 0, "AC4: AC1 runtime probe present")
    must(tst.find("--- #4163 AC2:") >= 0, "AC4: AC2 runtime probe present")
    must(tst.find("--- #4163 AC3:") >= 0, "AC4: AC3 runtime probe present")
    must(tst.find("--- #4163 AC4:") >= 0, "AC4: AC4 source-cite probe present")
    must(tst.find("free_orphan_nodes_from") >= 0, "AC4: probes drive free_orphan_nodes_from")
    must(tst.find("flat->bump_generation()") >= 0, "AC4: AC1 forces the gen-change sync path")
    must(
        tst.find("invalidate_tag_arity_index_for_test") >= 0,
        "AC4: AC3 drives the cold full-rebuild path (invalidate + force_build)",
    )

    # Dispatch check: the runner must invoke the #4163 blocks (they live
    # inline inside run_test_query_index_composite, so brace-scope is the
    # dispatch — verify the runner body contains them by slicing).
    i = tst.find("int run_test_query_index_composite()")
    body = tst[i:] if i >= 0 else ""
    must(body.find("--- #4163 AC1:") >= 0, "AC4: AC1 dispatched by the runner (inline scope)")
    must(body.find("--- #4163 AC3:") >= 0, "AC4: AC3 dispatched by the runner (inline scope)")

    must(
        not (ROOT / "tests" / "compiler" / "test_issue_4163.cpp").exists(), "AC4: no tests/compiler/test_issue_4163.cpp"
    )
    stray = [p.name for p in (ROOT / "docs" / "design").glob("4163-*")] if (ROOT / "docs/design").exists() else []
    must(not stray, "AC4: no docs/design/4163-* markdown")
    must("check_query_index_free_slot_4163.py" in build, "AC4: build.py registers the linter")
    must("check_query_index_free_slot_4163.py" in allow, "AC4: root_check_allowlist.txt lists the linter")

    failed = [label for ok, label in checks if not ok]
    for ok, label in checks:
        print(f"{'PASS' if ok else 'FAIL'}: {label}")
    if failed:
        print(f"\ncheck_query_index_free_slot_4163: {len(failed)} row(s) FAILED")
        return 1
    print(f"\ncheck_query_index_free_slot_4163: all {len(checks)} rows pass")
    return 0


def _prim_window_qws(qws: str) -> str:
    """Slice the query:pattern index fast path (use_index_fast_path block)."""
    i = qws.find("if (use_index_fast_path) {")
    if i < 0:
        return ""
    j = qws.find("// Full walk (Kleene + ellipsis, or wildcard root).", i)
    return qws[i:j] if j > 0 else qws[i:]


if __name__ == "__main__":
    sys.exit(main())
