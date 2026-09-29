#!/usr/bin/env python3
"""Issue #4162 source-cite gate: query:* full-scan free/ghost orphan skip.

free_orphan_nodes_from (rollback / parse-failure cleanup, #1299/#1300)
zeroes node_gen_ only — tag_ / marker_ / children_ survive as tombstones.
Several Agent-facing query walks iterated `id < flat.size()` and matched
via `FlatAST::get(id)` WITHOUT is_free_slot, so under Production defaults
the #4088/#3286 schema-2 auto-upgrade stamped epoch-fresh tombstone
NodeIds into QueryResults; every later mutate resolve then failed
stale-ref (poisoned multi-round Agent memory).

Fix shape: reuse the #1299/#1300 free-slot skip on every affected size()
walk (query:filter / query:calls / query:node-type /
query:defines-by-marker) and refuse free ids on the write side —
stamp_query_result_full_provenance requires is_live_node and
allow_query_stable_ref_export returns false for free slots (same face as
the read side, query_result_is_fresh_with_refs → StaleByEpoch).

ACs:
  AC1  query:filter walk guards is_free_slot before flat.get(id), cites
       #4162 (guard precedes the #425/#2525 hygiene gate so ghost slots
       do not pollute hygiene-skip counters).
  AC2  query:calls walk guards is_free_slot before flat.get(id) (0-arg
       and named forms share the single full-scan loop).
  AC3  query:node-type walk guards is_free_slot before the tag match.
  AC4  query:defines-by-marker walk guards is_free_slot before the
       tag+marker match.
  AC5  stamp_query_result_full_provenance refuses non-live ids
       (flat.is_live_node) BEFORE the #3198 export-gate consult, cites
       #4162; the stale `(void)flat;` no-op is gone.
  AC6  allow_query_stable_ref_export (evaluator_security.cpp) refuses
       free slots (ws->is_free_slot → return false) before the eager/
       torn ladder, cites #4162 — export_ref / export_ref_safe /
       stamp_query_stable_ref_export inherit the refusal.
  AC7  test home + registry: test_query_result_full_provenance.cpp hosts
       the ac4162 runtime probes + source-cite AC and dispatches them;
       no tests/compiler/test_issue_4162.cpp; no docs/design/4162-*;
       build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists
       check_query_free_slot_4162.py.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QWS = ROOT / "src" / "compiler" / "evaluator_primitives_query_workspace.cpp"
SEC = ROOT / "src" / "compiler" / "evaluator_security.cpp"
TST = ROOT / "tests" / "compiler" / "test_query_result_full_provenance.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

checks: list[tuple[bool, str]] = []


def must(cond: bool, label: str) -> None:
    checks.append((bool(cond), label))


def _prim_window(text: str, start_anchor: str) -> str:
    """Slice from a prim registration to the next sibling `\\n    add(`."""
    i = text.find(start_anchor)
    if i < 0:
        return ""
    j = text.find("\n    add(", i + len(start_anchor))
    return text[i:j] if j > 0 else text[i:]


def main() -> int:
    qws = QWS.read_text() if QWS.exists() else ""
    sec = SEC.read_text() if SEC.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOW.read_text() if ALLOW.exists() else ""

    must(bool(qws), "query_workspace TU readable")
    must(bool(sec), "evaluator_security TU readable")

    # ── AC1: query:filter walk guard ──
    w = _prim_window(qws, 'add("query:filter"')
    must("Issue #4162: skip free/ghost orphan slots" in w, "AC1: query:filter cites #4162")
    g = w.find("if (flat.is_free_slot(id))")
    v = w.find("auto v = flat.get(id);")
    h = w.find("Issue #425 / #2525: hygiene gate")
    must(g >= 0, "AC1: query:filter has is_free_slot guard")
    must(v >= 0 and (g < v), "AC1: guard precedes flat.get(id)")
    must(h >= 0 and (g < h), "AC1: guard precedes hygiene gate (no ghost counter pollution)")

    # ── AC2: query:calls walk guard ──
    w = _prim_window(qws, 'add("query:calls"')
    must("Issue #4162: skip free/ghost orphan slots" in w, "AC2: query:calls cites #4162")
    g = w.find("if (flat.is_free_slot(id))")
    v = w.find("auto v = flat.get(id);")
    must(g >= 0 and v >= 0 and (g < v), "AC2: query:calls guard precedes flat.get(id)")

    # ── AC3: query:node-type walk guard ──
    w = _prim_window(qws, 'add("query:node-type"')
    must("Issue #4162: skip free/ghost orphan slots" in w, "AC3: query:node-type cites #4162")
    g = w.find("if (flat.is_free_slot(id))")
    v = w.find("flat.get(id).tag == target_tag")
    must(g >= 0 and v >= 0 and (g < v), "AC3: query:node-type guard precedes tag match")

    # ── AC4: query:defines-by-marker walk guard ──
    w = _prim_window(qws, 'add("query:defines-by-marker"')
    must("Issue #4162: skip free/ghost orphan slots" in w, "AC4: query:defines-by-marker cites #4162")
    g = w.find("if (flat.is_free_slot(id))")
    v = w.find("auto v = flat.get(id);")
    must(g >= 0 and v >= 0 and (g < v), "AC4: defines-by-marker guard precedes flat.get(id)")

    # Contrast pinned by the issue body: the #1299/#1300 skips survive.
    must(
        qws.find("Issue #1299/#1300: skip free/ghost orphan slots after rollback.") >= 0,
        "contrast: #1299/#1300 query:defines skip intact",
    )

    # ── AC5: stamp helper refuses non-live ids ──
    # End anchor is the NEXT function signature — the stamp window's own
    # #4162 comment cites query_result_is_fresh_with_refs, so that string
    # cannot serve as the boundary.
    i = qws.find("stamp_query_result_full_provenance(aura::core::QueryResult& qr")
    j = qws.find("static aura::core::QueryResultFreshness", i + 1 if i >= 0 else 0)
    w = qws[i:j] if i >= 0 and j > i else ""
    must(bool(w), "AC5: stamp helper window found")
    must("Issue #4162: refuse free/ghost orphan slots" in w, "AC5: stamp helper cites #4162")
    lv = w.find("flat.is_live_node(static_cast<aura::ast::NodeId>(nid))")
    ex = w.find("ev.allow_query_stable_ref_export(nid)")
    must(lv >= 0 and ex >= 0 and (lv < ex), "AC5: is_live_node refusal precedes export-gate consult")
    must(w.find("return false") > lv, "AC5: refusal is fail-closed (return false)")
    must("(void)flat;" not in w, "AC5: flat parameter is now used (no `(void)flat;` no-op)")

    # ── AC6: export gate refuses free slots ──
    i = sec.find("bool Evaluator::allow_query_stable_ref_export(ast::NodeId id)")
    j = sec.find("restamp_hot_cone_held_overflow()", i + 1 if i >= 0 else 0)
    w = sec[i:j] if i >= 0 and j > i else ""
    must(bool(w), "AC6: export-gate window found")
    must("Issue #4162" in w, "AC6: export gate cites #4162")
    must("ws->is_free_slot(id)" in w, "AC6: export gate consults is_free_slot")
    gi = w.find("ws->is_free_slot(id)")
    ri = w.find("return false", gi if gi >= 0 else 0)
    must(gi >= 0 and ri >= 0 and (gi < j), "AC6: free slot → return false (fail-closed)")
    must(gi >= 0 and ri < j, "AC6: refusal sits before the eager/torn ladder")

    # ── AC7: test home + registry ──
    for fn in (
        "test_ac4162_1_prod_filter_skips_ghost_define",
        "test_ac4162_2_prod_calls_skip_ghost_call",
        "test_ac4162_3_prod_node_type_skips_ghost_define",
        "test_ac4162_4_prod_defines_by_marker_skips_ghost",
        "test_ac4162_5_stamp_and_export_refuse_tombstone",
        "test_ac4162_6_source_cite",
    ):
        must(f"void {fn}()" in tst, f"AC7: {fn} defined")
        must(f"{fn}();" in tst, f"AC7: {fn} dispatched")
    must(
        "4162 AC5: tombstone match NOT Fresh (StaleByEpoch)" in tst,
        "AC7: freshness-with-refs tombstone runtime CHECK present",
    )
    must("check_query_free_slot_4162.py" in build, "AC7: build.py registers this linter")
    must("check_query_free_slot_4162.py" in allow, "AC7: root_check_allowlist.txt lists this linter")
    must(
        not (ROOT / "tests" / "compiler" / "test_issue_4162.cpp").exists(), "AC7: no tests/compiler/test_issue_4162.cpp"
    )
    must(not list((ROOT / "docs" / "design").glob("4162-*")), "AC7: no docs/design/4162-* markdown")

    failed = 0
    for ok, label in checks:
        if ok:
            print(f"  ok   {label}")
        else:
            failed += 1
            print(f"  FAIL {label}")
    print(f"check_query_free_slot_4162: {len(checks) - failed}/{len(checks)} rows green")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
