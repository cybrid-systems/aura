#!/usr/bin/env python3
"""Issue #3658: type dirty txn + mirror before IR cascade for non-rebind.

Guard Phase-5 densify is not a type txn. rebind/set-body already run
infer_flat_partial_with_dirty_txn. replace-subtree / lockless / move-node
did not, so IR cascade had dep/IR edges without a post-infer type cone.

Complementary to #3655 (persist SDO vs type∪IR cone). Do not treat
#3655 as covering this face.

Contract (one row per AC):
  AC1  Production + mutate:replace-subtree → type_dirty_txn_this_boundary
       + run_post_mutate_typecheck before persist/cascade
  AC2  rebind in-body typecheck kept; Guard skips when flag set; #2516
       phase order intact
  AC3  vacuous (no dirty/log) skips extra mirror
  AC4  Soft does not force type txn
  AC5  extends test_type_dirty_cone_dep_graph / type_dirty_txn_order;
       linter AFTER #3657; no test_issue_3658.cpp; no docs/design/;
       no query:incremental-relower-stats / query:dirty-cascade-stats rewrite

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

    dtor = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    tci = _read("src/compiler/type_checker_impl.cpp")
    cone = _read("tests/compiler/test_type_dirty_cone_dep_graph.cpp")
    ord_ = _read("tests/compiler/test_type_dirty_txn_order.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    win_at = dtor.find("must complete this-boundary type dirty txn")
    if win_at < 0:
        win_at = dtor.find("Issue #3658: Production/Full outermost success structural mutate")
    win = dtor[win_at : win_at + 2400] if win_at >= 0 else ""

    must("Issue #3658", "AC1 Guard cite", dtor)
    must("type_dirty_txn_this_boundary()", "AC1 skip flag", dtor)
    must("run_post_mutate_typecheck_no_lock()", "AC1 typecheck", win)
    must("type_dirty_txn_this_boundary_", "AC1 field", ixx)
    must("note_type_dirty_txn_this_boundary", "AC1 typecheck notes txn", tc)
    must("ac3658_1_replace_subtree_mirrors_cone", "AC1 cone suite", cone)

    must("mutate:rebind", "AC2 rebind", mut)
    must("run_post_mutate_typecheck_no_lock()", "AC2 rebind typecheck", mut)
    must("type_dirty_txn_phase1_invalidate_total", "AC2 #2516 phase1", tci)
    must("type_dirty_txn_phase2_reinfer_total", "AC2 #2516 phase2", tci)
    must("type_dirty_txn_phase3_mirror_total", "AC2 #2516 phase3", tci)
    must("ac3658_2_rebind_no_double_txn", "AC2 cone suite", cone)
    p1 = tci.find("type_dirty_txn_phase1_invalidate_total")
    p2 = tci.find("type_dirty_txn_phase2_reinfer_total")
    p3 = tci.find("type_dirty_txn_phase3_mirror_total")
    if not (0 <= p1 < p2 < p3):
        fails.append("AC2: #2516 phase order broken")

    if "mark_dirty_upward_call_count()" not in win and "mutation_log_size()" not in win:
        fails.append("AC3: vacuous skip missing dirty/log delta")
    must("ac3658_3_vacuous_zero_extra", "AC3 cone suite", cone)

    must("production_defaults_active()", "AC4 Soft skip", win)
    must("ac3658_4_soft_no_force", "AC4 cone suite", cone)

    must("check_type_dirty_txn_before_cascade_3658", "AC5 build.py", build)
    prev = build.find("check_unslotted_string_edge_parity_3657")
    ours = build.find("check_type_dirty_txn_before_cascade_3658")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3657")
    if "schema-3658" in q:
        fails.append("AC5: added schema-3658 query key")
    must("query:incremental-relower-stats", "AC5 relower key retained", q)
    must("query:dirty-cascade-stats", "AC5 cascade key retained", q)
    must("Issue #3658", "AC5 txn-order cite", ord_)
    if _read("tests/compiler/test_issue_3658.cpp"):
        fails.append("AC5: test_issue_3658.cpp present")
    if _read("docs/design/3658-type-dirty-txn-cascade.md"):
        fails.append("AC5: docs/design/ exists")

    if fails:
        print("FAIL #3658 type_dirty_txn_before_cascade:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3658 type_dirty_txn_before_cascade")
    return 0


if __name__ == "__main__":
    sys.exit(main())
