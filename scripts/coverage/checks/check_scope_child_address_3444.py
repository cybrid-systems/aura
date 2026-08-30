#!/usr/bin/env python3
"""Issue #3444: orch:scope-child returns path; spawn/watch/join/resolve accept it.

#2631 exposed spawn_child but discarded the child reference, so later
prims still targeted the per-Evaluator root. Addressing key reuses
directory_snapshot scope_path. One Evaluator map. HardDeny / dangling
scheduler → ok=#f. No AgentRegistry, no second per-child map.

Contract:
  AC1 orch:scope-child hash has scope-path / child-index
  AC2 scope-spawn :path creates the handle on the child
  AC3 omit path → root (today)
  AC4 HardDeny stub / dead scheduler → structured fail
  AC5 no AgentRegistry; no second Evaluator map
  AC6 extend existing orch tests; no test_issue_3444.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _block(hay: str, start_lit: str, end_lit: str) -> str:
    a = hay.find(start_lit)
    if a < 0:
        return ""
    b = hay.find(end_lit, a + 1) if end_lit else len(hay)
    return hay[a : b if b > a else len(hay)]


def main() -> int:
    fails: list[str] = []

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    scope_h = _read("src/orch/agent_scope.h")
    readme = _read("src/orch/README.md")
    t_hier = _read("tests/orch/test_agent_scope_hierarchy.cpp")
    t_orch = _read("tests/orch/test_orch_scope.cpp")
    t_child = _read("tests/compiler/test_orch_scope_child.cpp")
    build = _read("build.py")

    must("kScopeChildAddressIssue = 3444" in scope_h, "AC1: stamp in agent_scope.h")
    must("resolve_scope_path" in scope_h, "AC1: resolve_scope_path")
    must("format_child_scope_path" in scope_h, "AC1: format_child_scope_path")
    must("child_at" in scope_h, "AC1: child_at stays")
    must("Issue #3444" in agent, "AC1: prim cites #3444")

    child_b = _block(agent, 'add("orch:scope-child"', 'add("orch:scope-watch"')
    must("child-index" in child_b, "AC1: scope-child hash child-index")
    must("scope-path" in child_b, "AC1: scope-child hash scope-path")
    must("parent.spawn_child" in child_b, "AC1: still calls parent.spawn_child")
    must("(void)parent.spawn_child" not in child_b, "AC1: no longer discards child")
    must("&child == &parent" in child_b, "AC4: HardDeny stub check")
    must("scheduler_alive" in child_b, "AC4: dangling scheduler fail")

    spawn_b = _block(agent, 'add("orch:scope-spawn"', 'add("orch:scope-child"')
    watch_b = _block(agent, 'add("orch:scope-watch"', 'add("orch:scope-join-all"')
    join_b = _block(agent, 'add("orch:scope-join-all"', 'add("orch:scope-cancel-all"')
    resolve_b = _block(agent, 'add("orch:scope-resolve"', 'add("orch:agent-directory"')
    must("parse_scope_addr_kw" in spawn_b, "AC2: spawn parses :path")
    must("parse_scope_addr_kw" in watch_b, "AC2: watch parses :path")
    must("parse_scope_addr_kw" in join_b, "AC2: join-all parses :path")
    must("parse_scope_addr_kw" in resolve_b, "AC2: resolve parses :path")
    must("resolve_scope_addr" in spawn_b, "AC2: spawn resolves child")
    must("scope == root && scope->empty()" in join_b, "AC2: join child does not drop root")

    must("class AgentRegistry" not in agent, "AC5: no AgentRegistry in prims")
    must("class AgentRegistry" not in scope_h, "AC5: no AgentRegistry in agent_scope.h")
    must("g_evaluator_agent_scopes" in scope_h, "AC5: still one per-Evaluator map")
    must("#3444" in readme, "AC5: README cites #3444")
    must("check_scope_child_address_3444" in build, "AC6: build.py wires linter")
    must("3444 AC1" in t_hier, "AC6: test_agent_scope_hierarchy extended")
    must("3444 AC1" in t_orch, "AC6: test_orch_scope extended")
    must("3444 AC1" in t_child, "AC6: test_orch_scope_child extended")
    must(not (ROOT / "tests/orch/test_issue_3444.cpp").is_file(), "AC6: no invent orch")
    must(not (ROOT / "tests/compiler/test_issue_3444.cpp").is_file(), "AC6: no invent compiler")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3444-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    lint = _block(build, "def cmd_lint", "def cmd_")
    i3457 = lint.find("check_eval_flat_sym_intern_3457")
    i3444 = lint.find("check_scope_child_address_3444")
    must(i3457 >= 0 and i3444 > i3457, "AC6: cmd_lint sequential after #3457")

    if fails:
        print("FAIL #3444 scope_child_address:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3444 scope_child_address: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
