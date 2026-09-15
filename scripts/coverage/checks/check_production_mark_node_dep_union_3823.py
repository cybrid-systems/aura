#!/usr/bin/env python3
"""Issue #3823: Production mark_define_dirty unions node-dep dependents.

#3474 walks string called_by FIFO only. Node-only callers (Soft-erased
hole / inject / lockless fork) stayed clean until #3761 peel — a
lookup_define_v2 between mark and peel could clean-hit pre-mutate IR.

Fix: after #3474 cone mark on Production facade early-return, union
node-dep dependents of encode_fn_node(slot(name)) via the #3761 decode
walk into the same body-dirty set. Soft inject remains observe-only.

Contract:
  AC1  helper + both facade-success call sites after #3474 cone
  AC2  Soft path does not call helper (before Soft gc_coord body)
  AC3  soak / live tests: mark × lookup no clean-hit; Soft observe-only
  AC4  no invent test_issue_3823 / docs/design; build.py wired

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
    test_cascade = _read("tests/compiler/test_dep_graph_hybrid_cascade.cpp")
    test_facade = _read("tests/compiler/test_compiler_hot_update_facade.cpp")
    build = _read("build.py")

    must("mark_node_dep_dependents_body_dirty_", "AC1 decl", ixx)
    must("void CompilerService::mark_node_dep_dependents_body_dirty_", "AC1 def", svc)
    hpos = svc.find("void CompilerService::mark_node_dep_dependents_body_dirty_")
    hwin = svc[hpos : hpos + 2400] if hpos >= 0 else ""
    must("Issue #3823", "AC1 cite", hwin)
    must("encode_fn_node", "AC1 encode", hwin)
    must("decode_fn_slot", "AC1 #3761 decode", hwin)
    must("is_block_dep_node", "AC1 block decode", hwin)
    must("mark_caller_body_dirty", "AC1 #3474 union", hwin)
    must("ir_cache_v2_.empty()", "AC1 empty-cache no-op", hwin)
    must_not("dep_graph_.erase(", "AC1 no erase", hwin)
    must_not("rebuild_node_dep_graph_from_string", "AC1 no remirror", hwin)
    must_not("dep_graph_generation_.fetch_add", "AC1 no generation", hwin)

    md = _fn_win(svc, "void CompilerService::mark_define_dirty")
    inv = _fn_win(svc, "void CompilerService::invalidate_function")
    must("mark_node_dep_dependents_body_dirty_(name)", "AC1 mark_define_dirty", md)
    must("mark_node_dep_dependents_body_dirty_(name)", "AC1 invalidate_function", inv)
    cone = md.find("mark_called_by_cone_body_dirty_(name)")
    node = md.find("mark_node_dep_dependents_body_dirty_(name)")
    soft = md.find("gc_coord::Scope gc_coord_scope")
    if cone < 0 or node < 0 or node < cone:
        fails.append("AC1: node-dep helper must run after #3474 cone mark")
    if soft < 0 or node > soft:
        fails.append("AC2: node-dep helper must be before Soft BFS body")

    must("3823 AC1: lookup_define_v2(g)==1 needs-relower before peel", "AC3 live", test_cascade)
    must("3823 soak: lockless one-side write × mark × lookup does not clean-hit", "AC3 soak", test_cascade)
    must("3823 AC2: helper only on Production facade path (before Soft body)", "AC2 soft", test_cascade)
    must("3823: node-dep union after #3474 cone mark", "AC3 facade cite", test_facade)

    must("check_production_mark_node_dep_union_3823", "AC4 build.py", build)
    must("check_production_called_by_cone_bfs_3474", "AC4 #3474 still wired", build)
    must("pmndu3823_script", "AC4 lint-batch var", build)

    must_not("schema-3823", "AC4 no query key", svc + ixx)
    must_not("g_3823_", "AC4 no g_3823_*", svc + ixx)
    if (ROOT / "tests" / "compiler" / "test_issue_3823.cpp").is_file():
        fails.append("AC4: test_issue_3823.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3823.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3823.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3823-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    if fails:
        print("FAIL #3823 production_mark_node_dep_union:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3823 production_mark_node_dep_union: mark-time node-dep union; Soft observe-only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
