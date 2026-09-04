#!/usr/bin/env python3
"""Issue #3255: Soft dual-graph parity fail fail-closed before partial peel.

Production/Strict (#3187) already force-dirties all callers on dual-graph
parity fail. Soft / non-Strict only rebuilt; a concurrent fiber / lockless
batch that left the graphs divergent can still reach
relower_dirty_defines_from_workspace with want_partial=true and peel under
an incomplete hybrid cascade cone.

#3255 re-checks graphs_consistent at the partial decision (shared lock
walk only when already consistent). On fail: exclusive rebuild, bump
dual_dep_graph_parity_fail_total, force-dirty all callers, want_partial=false
+ mark_all_blocks_dirty. Distinguisher: existing
partial_forced_full_by_impact_total (no new metric key).

Contract:
  AC1  relower_dirty_defines_from_workspace re-checks graphs_consistent
       before prepare_source_to_ir_map_for_partial_ / peel
  AC2  Production/Strict record_dependency + drain still use
       dual_dep_graph_strict_or_production + all-callers walk
  AC3  clean Soft: exclusive rebuild only on fail (shared-lock walk on
       consistent); no extra exclusive on quiet path
  AC4  dual_dep_graph_parity_fail_total still increments; distinguisher
       reuses partial_forced_full_by_impact_total
  AC5  test_dep_graph_hybrid_cascade.cpp extended with ac3255_*
  AC6  linter wired in build.py after #3254; no docs/design/3255-* (#1655);
       no tests/issues/test_issue_3255.cpp (#81967)

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

    svc = _read("src/compiler/service.ixx")
    obs = _read("src/compiler/observability_metrics.h")
    q_obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    q_std = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    t = _read("tests/compiler/test_dep_graph_hybrid_cascade.cpp")
    build = _read("build.py")

    helper_pos = svc.find("fail_closed_soft_dual_graph_parity_before_partial_")
    helper_win = svc[max(0, helper_pos - 2000) : helper_pos + 3500] if helper_pos >= 0 else ""
    rel_pos = svc.find("std::size_t relower_dirty_defines_from_workspace()")
    # Window expanded after #3381/#3484 grew the peel body (caller union
    # + zero-mask fail-closed before want_partial / prepare_source_to_ir).
    rel_win = svc[rel_pos : rel_pos + 20000] if rel_pos >= 0 else ""
    want_pos = rel_win.find("if (want_partial && dirty_n > 0)")
    want_win = rel_win[want_pos : want_pos + 2500] if want_pos >= 0 else ""

    must("Issue #3255", "AC1 helper cite", helper_win)
    must("graphs_consistent", "AC1 peel re-check", helper_win)
    must("fail_closed_soft_dual_graph_parity_before_partial_", "AC1 call", want_win)
    must("prepare_source_to_ir_map_for_partial_", "AC1 before peel", want_win)
    if want_win.find("fail_closed_soft_dual_graph_parity_before_partial_") > want_win.find(
        "prepare_source_to_ir_map_for_partial_"
    ):
        fails.append("AC1: Soft parity helper must run BEFORE prepare_source_to_ir_map_for_partial_")
    must("rebuild_node_dep_graph_from_string", "AC1 rebuild on fail", helper_win)
    must("want_partial = false", "AC1 fail-closed", helper_win)
    must("mark_all_blocks_dirty", "AC1 mark dirty", helper_win)
    must("for (const auto& [callee_name, callee_entry] : dep_graph_)", "AC1 all-callers", helper_win)
    must("ac3255_1_soft_fork_forces_full", "AC1 test", t)

    rd_pos = svc.find("void record_dependency(const std::string& caller")
    if rd_pos < 0:
        rd_pos = svc.find("void record_dependency(")
    rd_win = svc[rd_pos : rd_pos + 9000] if rd_pos >= 0 else ""
    drain_pos = svc.find("void drain_deferred_hybrid_cascade_()")
    drain_win = svc[drain_pos : drain_pos + 5500] if drain_pos >= 0 else ""
    must("dual_dep_graph_strict_or_production()", "AC2 record", rd_win)
    must("for (const auto& [callee_name, callee_entry] : dep_graph_)", "AC2 record walk", rd_win)
    must("dual_dep_graph_strict_or_production()", "AC2 drain", drain_win)
    must("for (const auto& [callee_name, callee_entry] : dep_graph_)", "AC2 drain walk", drain_win)
    must("ac3255_2_production_unchanged", "AC2 test", t)

    must("OrderedSharedLock", "AC3 shared walk", helper_win)
    must("OrderedUniqueLock", "AC3 exclusive on fail", helper_win)
    shared_pos = helper_win.find("OrderedSharedLock")
    unique_pos = helper_win.find("OrderedUniqueLock")
    if shared_pos < 0 or unique_pos < 0 or unique_pos < shared_pos:
        fails.append("AC3: exclusive lock must follow shared graphs_consistent walk")
    if "if (graphs_ok)" not in helper_win:
        fails.append("AC3: exclusive rebuild must return early when graphs already consistent")
    must("ac3255_3_clean_soft_zero_extra", "AC3 test", t)

    must("dual_dep_graph_parity_fail_total", "AC4 fail_total", helper_win)
    must("partial_forced_full_by_impact_total", "AC4 distinguisher", helper_win)
    must("Issue #3255", "AC4 obs cite", obs)
    must("schema-3255", "AC4 schema obs", q_obs)
    must("schema-3255", "AC4 schema sweep", q_std)
    if "dual_dep_graph_parity_forced_full_total" in obs:
        fails.append("AC4: new dual_dep_graph_parity_forced_full_total (reuse existing distinguisher)")
    must("ac3255_4_metrics_soak_and_linter", "AC4/AC5 test", t)

    must("ac3255_1_soft_fork_forces_full();", "AC5 main", t)
    must("ac3255_2_production_unchanged();", "AC5 main", t)
    must("ac3255_3_clean_soft_zero_extra();", "AC5 main", t)
    must("ac3255_4_metrics_soak_and_linter();", "AC5 main", t)
    must("#3255: Soft dual-graph parity fail", "AC5 marker", t)

    must("check_dual_dep_graph_soft_parity_partial_3255", "AC6 build.py", build)
    nfe = build.find("check_hold_budget_noncoop_force_edge_3254")
    ours = build.find("check_dual_dep_graph_soft_parity_partial_3255")
    if nfe < 0 or ours < 0 or ours < nfe:
        fails.append("AC6: linter must be wired in build.py AFTER #3254 linter")

    if (ROOT / "tests" / "issues" / "test_issue_3255.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3255.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3255.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3255.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3255-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3255 dual_dep_graph_soft_parity_partial:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3255 dual_dep_graph_soft_parity_partial: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
