#!/usr/bin/env python3
"""Issue #3345: production hybrid depth-1 called_by IR dirty after facade.

Production mark_define_dirty / invalidate_function early-return after
hard_invalidate_via_facade (#3112/#3150/#3188/#3219) and skip Soft
dep_graph BFS. Hybrid interpreter dependents can exec unmarked IR.
Fix: after facade IR/shape step, mark direct called_by body-only dirty
when IR cache is live. Empty IR cache → no dep_graph lock. Soft BFS
unchanged. No new query keys.

Contract:
  AC1  helper + both production facade-success call sites; depth-1
       called_by (no BFS queue)
  AC2  hybrid production: direct dependent body-only dirty (test AC2)
  AC3  Soft BFS (hybrid_node_cascade_ + drain + queue) unchanged
  AC4  #3112/#3150/#3188/#3219 preserved; no new query:*
  AC5  test_compiler_hot_update_facade; linter AFTER #3219; no invent /
       docs/design/; no schema-3345 / g_3345_*

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

    svc = _read("src/compiler/service_dirty.cpp")
    ixx = _read("src/compiler/service.ixx")
    hur = _read("src/compiler/hot_update_registry.cpp")
    test = _read("tests/compiler/test_compiler_hot_update_facade.cpp")
    build = _read("build.py")

    must("mark_direct_hybrid_dependents_body_dirty_", "AC1 decl", ixx)
    must("void CompilerService::mark_direct_hybrid_dependents_body_dirty_", "AC1 def", svc)
    hpos = svc.find("void CompilerService::mark_direct_hybrid_dependents_body_dirty_")
    hwin = svc[hpos : hpos + 2200] if hpos >= 0 else ""
    must("Issue #3345", "AC1 cite", hwin)
    must("called_by", "AC1 depth-1", hwin)
    must("ir_cache_v2_.empty()", "AC1 empty-cache no-op", hwin)
    must("mark_body_only_dirty", "AC1 body-only", hwin)
    if "std::queue" in hwin:
        fails.append("AC1: helper must not run Soft BFS queue")

    md = _fn_win(svc, "void CompilerService::mark_define_dirty")
    inv = _fn_win(svc, "void CompilerService::invalidate_function")
    must("mark_direct_hybrid_dependents_body_dirty_(name)", "AC1 mark_define_dirty", md)
    must("mark_direct_hybrid_dependents_body_dirty_(name)", "AC1 invalidate_function", inv)
    facade = md.find("hard_invalidate_via_facade(")
    fanout = md.find("mark_direct_hybrid_dependents_body_dirty_(name)")
    soft = md.find("gc_coord::Scope gc_coord_scope")
    if facade < 0 or fanout < 0 or fanout < facade:
        fails.append("AC1: mark_define_dirty helper must be after facade success")
    if soft < 0 or fanout > soft:
        fails.append("AC3: helper must be before Soft BFS body")

    must("3345 AC2", "AC2 test", test)

    must("hybrid_node_cascade_", "AC3 Soft hybrid", svc)
    must("drain_deferred_hybrid_cascade_", "AC3 Soft drain", svc)
    must("std::queue<std::string> bfs", "AC3 Soft BFS", svc)

    must("hard_invalidate_via_facade(", "AC4 #3112", svc)
    must("stamp_eval_core_joint_after_production_facade_(name)", "AC4 #3219", svc)
    must("Issue #3188 AC1: residual of #3150", "AC4 #3188", svc)
    must("notify_dirty_define(name)", "AC4 #3150", hur)
    must("check_production_hybrid_depth1_fanout_3345", "AC5 build.py", build)
    must("ac3345_production_hybrid_depth1_fanout", "AC5 test", test)
    prev = build.find("check_eval_core_joint_after_production_facade_3219")
    ours = build.find("check_production_hybrid_depth1_fanout_3345")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3219")
    if "schema-3345" in svc or "schema-3345" in ixx:
        fails.append("AC5: new schema-3345 query key")
    if "g_3345_" in svc or "g_3345_" in ixx:
        fails.append("AC5: new g_3345_* counter")
    if _read("tests/compiler/test_issue_3345.cpp"):
        fails.append("AC5: test_issue_3345.cpp present (forbidden #81967)")
    if _read("docs/design/3345-hybrid-depth1-fanout.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3345 production_hybrid_depth1_fanout:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3345 production_hybrid_depth1_fanout: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
