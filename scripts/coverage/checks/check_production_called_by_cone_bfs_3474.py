#!/usr/bin/env python3
"""Issue #3474: production called_by cone is FIFO / transitive.

#3381 unioned one hop of dep_graph_[root].called_by at peel. #3345 marks
direct called_by after facade. Chain f ← g ← h left h clean under
production. Fix: FIFO called_by snapshot (same walk Soft uses) at peel
and after facade success. Shared lock only — no dep_graph erase, no
generation bump. Soft invalidate teardown unchanged. No new query keys.

Contract:
  AC1  helper + both facade-success call sites; peel FIFO deque walk
  AC2  peel marks callers / take-full; never silent skip
  AC3  Soft BFS erase+generation unchanged; helper before Soft body
  AC4  dual-graph Strict force-dirty path untouched
  AC5  #3345 helper stays direct-only (no queue); extend existing suites
  AC6  no new query key / g_3474_* / test_issue_3474 / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _fn_win(src: str, sig: str) -> str:
    pos = src.find(sig)
    if pos < 0:
        return ""
    nxt = src.find("\nvoid CompilerService::", pos + 1)
    return src[pos:nxt] if nxt > pos else src[pos : pos + 12000]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    svc = _read("src/compiler/service_dirty.cpp")
    ixx = _read("src/compiler/service.ixx")
    test_facade = _read("tests/compiler/test_compiler_hot_update_facade.cpp")
    test_peel = _read("tests/compiler/test_incremental_facade_dirty_names_snapshot.cpp")
    build = _read("build.py")

    must("mark_called_by_cone_body_dirty_", "AC1 decl", ixx)
    must("void CompilerService::mark_called_by_cone_body_dirty_", "AC1 def", svc)
    hpos = svc.find("void CompilerService::mark_called_by_cone_body_dirty_")
    hwin = svc[hpos : hpos + 2800] if hpos >= 0 else ""
    must("Issue #3474", "AC1 cite", hwin)
    must("std::deque<std::string> bfs", "AC1 FIFO deque", hwin)
    must("bfs.pop_front()", "AC1 pop_front", hwin)
    must("called_by", "AC1 called_by", hwin)
    must("mark_caller_body_dirty", "AC1 mark", hwin)
    must("ir_cache_v2_.empty()", "AC1 empty-cache no-op", hwin)
    must_not("dep_graph_.erase(", "AC5 helper no erase", hwin)
    must_not("dep_graph_generation_.fetch_add", "AC5 helper no generation", hwin)

    md = _fn_win(svc, "void CompilerService::mark_define_dirty")
    inv = _fn_win(svc, "void CompilerService::invalidate_function")
    must("mark_called_by_cone_body_dirty_(name)", "AC1 mark_define_dirty", md)
    must("mark_called_by_cone_body_dirty_(name)", "AC1 invalidate_function", inv)
    d1 = md.find("mark_direct_hybrid_dependents_body_dirty_(name)")
    cone = md.find("mark_called_by_cone_body_dirty_(name)")
    soft = md.find("gc_coord::Scope gc_coord_scope")
    if d1 < 0 or cone < 0 or cone < d1:
        fails.append("AC5: cone helper must run after #3345 depth-1 helper")
    if soft < 0 or cone > soft:
        fails.append("AC3: cone helper must be before Soft BFS body")

    rel_pos = ixx.find("std::size_t relower_dirty_defines_from_workspace()")
    peel = ixx[rel_pos : rel_pos + 18000] if rel_pos >= 0 else ""
    b3381 = peel.find("Issue #3381")
    block = peel[b3381 : b3381 + 9000] if b3381 >= 0 else ""
    must("Issue #3474", "AC1 peel cite", block)
    must("std::deque<std::string> bfs", "AC1 peel FIFO", block)
    must("bfs.pop_front()", "AC1 peel pop_front", block)
    must("dep_graph_.find(current)", "AC1 peel current", block)
    must("dit->second.called_by", "AC1 peel called_by", block)
    must("mark_caller_body_dirty", "AC1 peel mark", block)
    must("dirty_names.push_back(caller)", "AC2 peel append", block)
    must("finish_cascade_soa_dirty_sync_", "AC1 peel soa", block)
    must_not("dep_graph_.erase(", "AC5 peel no erase", block)
    must_not("dep_graph_generation_.fetch_add", "AC5 peel no generation", block)
    must("partial_forced_full_by_impact_total", "AC2 take-full", block)

    h3345 = svc.find("void CompilerService::mark_direct_hybrid_dependents_body_dirty_")
    n3345 = svc.find("\nvoid CompilerService::", h3345 + 1) if h3345 >= 0 else -1
    w3345 = svc[h3345:n3345] if h3345 >= 0 and n3345 > h3345 else ""
    if "std::queue" in w3345 or "std::deque" in w3345:
        fails.append("AC5: #3345 helper must stay direct-only (no BFS queue/deque)")

    parity = ixx.find("void fail_closed_soft_dual_graph_parity_before_partial_")
    pwin = ixx[parity : parity + 3500] if parity >= 0 else ""
    must("mark_all_blocks_dirty", "AC4 dual-graph", pwin)
    must("called_by", "AC4 dual-graph callers", pwin)

    must("hybrid_node_cascade_", "AC3 Soft hybrid", svc)
    must("drain_deferred_hybrid_cascade_", "AC3 Soft drain", svc)
    must("dep_graph_generation_.fetch_add", "AC3 Soft generation", svc)

    must("3474 AC1: transitive h dirty before peel", "AC5 live h", test_facade)
    must("3474 AC1: cone k dirty before peel", "AC5 live k", test_facade)
    must("3474 AC5: #3381 direct caller g still dirty", "AC5 direct-only", test_facade)
    must("Issue #3474", "AC5 peel test", test_peel)
    must("std::deque<std::string> bfs", "AC5 peel test FIFO", test_peel)

    must("check_production_called_by_cone_bfs_3474", "AC5 build.py", build)
    prev = build.find("check_production_hybrid_depth1_fanout_3345")
    ours = build.find("check_production_called_by_cone_bfs_3474")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3345")

    must_not("schema-3474", "AC6 no query key", svc + ixx)
    must_not("g_3474_", "AC6 no g_3474_*", svc + ixx)
    if (ROOT / "tests" / "compiler" / "test_issue_3474.cpp").is_file():
        fails.append("AC6: test_issue_3474.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3474.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3474.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3474-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3474 production_called_by_cone_bfs:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3474 production_called_by_cone_bfs: FIFO cone; #3345 stays direct; Soft teardown kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
