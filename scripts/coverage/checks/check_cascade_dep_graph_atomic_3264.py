#!/usr/bin/env python3
"""Issue #3264: cascade dep-graph atomic + mutex + dropped-roots counter.

g_pipeline_dep_graph is an atomic pointer (release store / acquire load).
flush_pipeline_cascade_roots and set_pipeline_dep_graph share a mutex so
g_global_dirty writes cannot race the graph dereference. Unset graph with
pending TLS roots bumps cascade_roots_dropped_no_dep_graph_total. Empty-root
flush is a no-op (zero extra). Bug 4 visited-set perf deferred.

Contract:
  AC1  g_pipeline_dep_graph is atomic; set release / get+flush acquire
  AC2  mutex around set + flush dirty writes
  AC3  dropped counter on unset graph with pending roots
  AC4  empty-root flush zero extra
  AC5  extend existing suites; linter after #3263; no invent

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

    dirty = _read("src/compiler/dirty_propagation.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    skip = _read("tests/compiler/test_cascade_skip_metrics.cpp")
    cascade = _read("tests/compiler/test_dirty_propagation_cascade.cpp")
    build = _read("build.py")
    l3263 = _read("scripts/coverage/checks/check_quote_lambda_marker_once_3263.py")

    must("Issue #3264", "AC1 cite", dirty)
    must("std::atomic<const DepGraph*> g_pipeline_dep_graph", "AC1 atomic", dirty)
    spos = dirty.find("inline void set_pipeline_dep_graph")
    swin = dirty[spos : spos + 400] if spos >= 0 else ""
    must("g_pipeline_dep_graph.store(g, std::memory_order_release)", "AC1 release", swin)
    must("lock(g_pipeline_cascade_mtx)", "AC1 set lock", swin)
    gpos = dirty.find("inline const DepGraph* pipeline_dep_graph()")
    gwin = dirty[gpos : gpos + 250] if gpos >= 0 else ""
    must("g_pipeline_dep_graph.load(std::memory_order_acquire)", "AC1 get acquire", gwin)
    must("ac3264_1_atomic_graph_pointer", "AC1 test", skip)

    fpos = dirty.find("inline std::size_t flush_pipeline_cascade_roots()")
    fwin = dirty[fpos : fpos + 1800] if fpos >= 0 else ""
    must("std::mutex g_pipeline_cascade_mtx", "AC2 mutex", dirty)
    must("std::lock_guard<std::mutex> lock(g_pipeline_cascade_mtx)", "AC2 flush lock", fwin)
    must("g_pipeline_dep_graph.load(std::memory_order_acquire)", "AC2 flush acquire", fwin)
    must("ac3264_2_flush_mutex", "AC2 test", skip)

    must("cascade_roots_dropped_no_dep_graph_total", "AC3 counter", fwin)
    if "t_pipeline_cascade_roots.clear()" in fwin and "cascade_roots_dropped_no_dep_graph_total" not in fwin:
        fails.append("AC3: still silent-clear without dropped counter")
    must("cascade-roots-dropped-no-dep-graph-total", "AC3 query key", obs)
    must("ac3264_3_dropped_counter", "AC3 test", skip)

    must("t_pipeline_cascade_roots.empty()", "AC4 empty", fwin)
    must("zero extra", "AC4 comment", fwin)
    must("ac3264_4_quiet_empty_zero_extra", "AC4 test", skip)

    must("ac3264_5_source_and_linter", "AC5 test", skip)
    must("run_3264_source", "AC5 cascade family", cascade)
    must("check_cascade_dep_graph_atomic_3264", "AC5 build.py", build)
    prev = build.find("check_quote_lambda_marker_once_3263")
    ours = build.find("check_cascade_dep_graph_atomic_3264")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3263")
    must("3264", "AC5 extend 3263 linter", l3263)
    if (ROOT / "tests" / "issues" / "test_issue_3264.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3264.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3264.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3264.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3264-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3264" in q or "schema-3264" in skip or "schema-3264" in cascade:
        fails.append("AC5: new schema-3264 query key (SlimSurface)")

    if fails:
        print("FAIL #3264 cascade_dep_graph_atomic:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3264 cascade_dep_graph_atomic: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
