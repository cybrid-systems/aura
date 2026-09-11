#!/usr/bin/env python3
"""Issue #3643: optional tree join — join_all stays local by default.

Contract (one row per AC):
  AC1  AgentScope::join_all default (tree=false) is today's local
       contract (#3496 AC1 — root join leaves descendants live); the
       2-arg overload delegates with tree=false
  AC2  tree=true joins local handles first, then children_ in the same
       descendant order as cancel_all / directory_snapshot, folding the
       worst status (first non-Ok in walk order); each child applies its
       own on_join_fail
  AC3  orch:scope-join-all parses :tree #t and passes tree into
       scope->join_all; the handoff-required-style hash carries the
       tree flag + 3643 stamp; drop decision stays tree_settled root-only
       (#3496 AC3 untouched)
  AC4  Soft / default no :tree: zero new wait, zero new intern (no
       behavior change on the default path)
  AC5  ACs live in tests/orch/test_agent_scope_hierarchy.cpp (3643
       markers); no tests/**/test_issue_3643.cpp; no docs/design/3642-*;
       build.py wires check_scope_join_tree_3643 + root allowlist

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def forbid(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    scope = _read("src/orch/agent_scope.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_agent_scope_hierarchy.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: default local contract preserved ────────────────────────────
    must("kJoinAllTreeJoinIssue = 3643", "AC1 issue stamp", scope)
    must("Issue #3643", "AC1 join_all cites", scope)
    must("return join_all(policy, fail, /*tree=*/false);", "AC1 2-arg delegates local", scope)
    sig = scope.find("bool tree) {")
    rstart = scope.rfind("join_all(JoinPolicy policy", 0, sig) if sig >= 0 else -1
    if sig < 0 or rstart < 0:
        fails.append("AC2: 3-arg tree join_all missing")
    else:
        window = scope[rstart : sig + 2600]
        if "for (auto& c : children_)" not in window:
            fails.append("AC2: children_ walk missing")
        if "c->join_all(policy, fail, /*tree=*/true)" not in window:
            fails.append("AC2: child recursion missing")
        if "First non-Ok in walk order wins" not in window:
            fails.append("AC2: worst-status fold missing")
        if "apply_on_join_fail_unlocked_" not in window:
            fails.append("AC2: on_join_fail not applied per scope")

    # ── AC2/AC3: primitive :tree kw + pass-through ───────────────────────
    must('(k == "tree") && types::is_bool(val)', "AC3 :tree kw parse", prim)
    must("tree_join = types::as_bool(val);", "AC3 tree flag set", prim)
    must("scope->join_all(policy, fail_pol, tree_join)", "AC3 pass-through", prim)
    must("Issue #3643", "AC3 primitive cites", prim)
    must("schema-3643", "AC3 hash stamp", prim)
    must("per_handle_3643", "AC3 tree per-handle pass", prim)
    must("scope == root", "AC3 drop stays root-only", prim)
    must("scope->tree_settled()", "AC3 drop uses tree_settled", prim)

    # ── AC5: test placement + markers ────────────────────────────────────
    for n in (
        "3643: issue stamp",
        "3643 AC1: child fiber still live (local join)",
        "3643 AC2: tree join Ok",
        "3643 AC2: tree_settled true after tree join",
        "3643 AC3: root children intact after child-scope join (no drop)",
        "3643 AC6: build.py wires linter",
    ):
        must(n, "AC5 test marker", test)

    # ── AC4/AC5: no new files in forbidden slots ─────────────────────────
    if _read("tests/orch/test_issue_3643.cpp"):
        fails.append("AC5: tests/orch/test_issue_3643.cpp exists")
    if _read("tests/issues/test_issue_3643.cpp"):
        fails.append("AC5: tests/issues/test_issue_3643.cpp exists")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in sorted(design.glob("3643-*")):
            fails.append(f"AC5: docs/design file present: {p.name}")

    # ── AC5: gate wiring ─────────────────────────────────────────────────
    must("check_scope_join_tree_3643", "AC5 build.py wires linter", build)
    must("check_scope_join_tree_3643.py", "AC5 root allowlist", allow)

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("3643 scope join tree: all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
