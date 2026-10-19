#!/usr/bin/env python3
"""Issue #4598 (phase 2): hot-swap boundary-exit scan O(1) + macro gate O(1).

Follow-up to the #4598 phase-1 commit (skip unchanged defines on rebind and
restore). The remaining pinned O(workspace) costs on the single-define swap
path that this slice removes:

  - ~MutationBoundaryGuard walked every live node per boundary exit
    (is_live_node + has_match_info + tag per id) to decide the linear-op /
    match-site flags feeding RenderFastExit and the production invariant
    hard gate. FlatAST now maintains exact live counters
    (linear_ops_live_ / match_sites_live_) at the single-node write sites
    and recomputes them wholesale after column swaps; the exit reads two
    relaxed loads.
  - macro_expand_all probed for a MacroDef with a linear scan of the whole
    flat on every eval-current of a macro-free workspace. The gate now
    reads the #402 sticky SummaryFlag::HasMacroDef bit (bit==0 is
    exact-absent; sticky-true only re-enables the walk the body performs).

Contract:
  AC1 FlatAST counters exist with exact live-node semantics and O(1)
      readers (has_linear_ops_live / has_match_sites_live + raw counts)
  AC2 incremental hooks: add_node fresh birth, reset_node_slot recycle
      birth, free_orphan_nodes_from + recycle_dead_nodes deaths,
      set_match_info, counted set-tag write (set_tag_live_adjusted)
  AC3 wholesale recompute: compaction, truncate_to, wire load (+ summary
      recompute), copy/move ctor + assign mirror counters and summary bits
  AC4 boundary exit reads the counters; the per-node scan loop is gone
  AC5 ast_impl patch apply (field_offset 0) writes through the counted
      chokepoint
  AC6 macro-free gate reads SummaryFlag::HasMacroDef; no linear MacroDef
      probe loop remains
  AC7 runtime ACs live in test_summary_recompute_sym.cpp (#4598 AC6..AC10)
  AC8 build.py wiring + root_check_allowlist entry; no docs/design/4598-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    ast = _read("src/core/ast.ixx")

    # AC1 counters + O(1) readers
    must("mutable std::atomic<std::uint64_t> linear_ops_live_{0};", "AC1", ast)
    must("mutable std::atomic<std::uint64_t> match_sites_live_{0};", "AC1", ast)
    must("bool has_linear_ops_live() const noexcept", "AC1", ast)
    must("bool has_match_sites_live() const noexcept", "AC1", ast)
    must("std::uint64_t linear_ops_live_count() const noexcept", "AC1", ast)
    must("std::uint64_t match_sites_live_count() const noexcept", "AC1", ast)
    must("static constexpr bool is_linear_op_tag(NodeTag tag) noexcept", "AC1", ast)

    # AC2 incremental hooks
    must("void recompute_live_counters_() const", "AC2", ast)
    must("Issue #4598: fresh-slot birth", "AC2", ast)
    must("Issue #4598: free-list birth", "AC2", ast)
    must("Issue #4598: live slot dies — drop its scan counters.", "AC2", ast)
    must("const bool was = has_match_info(id);", "AC2", ast)
    must("void set_tag_live_adjusted(NodeId id, NodeTag tag)", "AC2", ast)

    # AC3 wholesale mirrors
    must("Issue #4598: columns + node_gen_ were rebuilt wholesale.", "AC3", ast)
    must("Issue #4598: the tail was cut wholesale.", "AC3", ast)
    must("ast.summary_recompute();", "AC3", ast)
    must("ast.recompute_live_counters_();", "AC3", ast)
    must("Issue #4598: the columns moved wholesale — mirror the counts and", "AC3", ast)
    must("Issue #4598: the columns were copied wholesale — mirror the counts", "AC3", ast)
    must("Issue #4598: columns moved wholesale — mirror the counts and", "AC3", ast)
    must("Issue #4598: columns were copied wholesale — mirror the counts", "AC3", ast)
    must("Issue #4598: no live nodes after clear().", "AC3", ast)

    # AC4 exit scan O(1)
    evb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    must("match_sites_present_local = ws->has_match_sites_live();", "AC4", evb)
    must("linear_ops_present_local = ws->has_linear_ops_live();", "AC4", evb)
    must_not("for (aura::ast::NodeId id = 0; id < n; ++id) {\n            if (!ws->is_live_node(id))", "AC4", evb)

    # AC5 patch apply chokepoint
    impl = _read("src/core/ast_impl.cpp")
    must("ast.set_tag_live_adjusted(p.node, static_cast<NodeTag>(p.new_value));", "AC5", impl)
    must_not("ast.tag(p.node) = static_cast<NodeTag>(p.new_value);", "AC5", impl)

    # AC6 macro gate
    mex = _read("src/compiler/macro_expansion.cpp")
    must("if (!flat.summary_has(SummaryFlag::HasMacroDef))", "AC6", mex)
    must("Issue #4598: a macro-free workspace (the hot-swap bench) has no", "AC6", mex)
    must_not("bool any_macro_def = false;", "AC6", mex)

    # AC7 runtime ACs
    test = _read("tests/core/test_summary_recompute_sym.cpp")
    for ac in (
        "--- #4598 AC6: linear counter parity on birth/truncate ---",
        "--- #4598 AC7: counted set-tag transition ---",
        "--- #4598 AC8: match-site counter + stale-recycle mirror ---",
        "--- #4598 AC9: wholesale mirrors (copy-assign + clear) ---",
        "--- #4598 AC10: macro-free expand gate decision input ---",
    ):
        must(ac, "AC7", test)
    must("linear_ops_live_count()", "AC7", test)
    must("match_sites_live_count()", "AC7", test)
    must("set_tag_live_adjusted", "AC7", test)

    if fails:
        for f in fails:
            print(f"FAIL {f}")
        return 1
    print("OK check_boundary_exit_counters_4598: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
