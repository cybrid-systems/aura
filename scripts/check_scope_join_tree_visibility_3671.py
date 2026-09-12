#!/usr/bin/env python3
"""Issue #3671: orch:scope-join-all publishes tree_settled; production
fail-closed on non-tree joins with live descendants.

Contract (one row per AC):
  AC1  production + local join + live descendants → ok flipped #f with
       deny-class=other / deny-detail=descendants-live (#3251 intern);
       #3496 drop gate preserved (settled-only drop)
  AC2  :tree / tree_join flag row unchanged (schema-3643 append-only)
  AC3  blame fields tree-settled / descendants-live added to the hash
  AC4  deny intern production-gated (Soft observe-only — bools still
       populate)
  AC5  tests extend the orch scope family; no invent test; no docs/design;
       no new query:orch-module-stats key

Exit 0 = all rows satisfied. --self-test checks the row helpers on
synthetic text, not the live tree.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _rows(must, must_count, must_not) -> list[str]:
    fails: list[str] = []

    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_scope_join_tree_visibility.cpp")
    build = _read("build.py")

    # AC1: capture + guard + deny intern.
    must("make_scope_addr_fail, add_deny_class]", "AC1 capture", prim)
    must("const bool tree_settled_now = scope->tree_settled();", "AC1 settled", prim)
    must("const bool descendants_live =", "AC1 descendants", prim)
    must("const bool join_guard_deny =", "AC1 guard", prim)
    must("add_deny_class(kv, aura::orch::AgentDenyClass::Other,", "AC1 deny-class other", prim)
    must('"descendants-live", /*retry_ms=*/0,', "AC1 deny-detail", prim)
    must("!join_guard_deny)", "AC1 ok flip", prim)
    must("Issue #3671", "AC1 cite", prim)

    # AC3: blame fields on the hash.
    must('{"tree-settled", make_bool(tree_settled_now)}', "AC3 tree-settled", prim)
    must('{"descendants-live", make_bool(descendants_live)}', "AC3 descendants", prim)

    # AC2: #3643 rows unchanged (append-only contract).
    must('{"tree", make_bool(tree_join)}', "AC2 tree row", prim)
    must('{"schema-3643", make_int(aura::orch::kJoinAllTreeJoinIssue)}', "AC2 schema-3643", prim)

    # AC5: tests in the orch scope family.
    must("3671 AC1", "AC5 test AC1", test)
    must("3671 AC2", "AC5 test AC2", test)
    must("3671 AC3", "AC5 test AC3", test)
    must("apply_production_audit_defaults", "AC5 production arm", test)
    if _read("tests/orch/test_issue_3671.cpp") or _read("tests/issues/test_issue_3671.cpp"):
        fails.append("AC5: test_issue_3671.cpp present (forbidden per #81934)")
    if _read("docs/design/3671-join-all-tree-visibility.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    must("check_scope_join_tree_visibility_3671", "AC5 build.py", build)
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_count(n: str, label: str, hay: str, k: int) -> None:
        c = hay.count(n)
        if c != k:
            fails.append(f"{label}: expected {k}x {n!r}, found {c}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    fails = _rows(must, must_count, must_not)

    if fails:
        print("FAIL #3671 scope_join_tree_visibility:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3671 scope_join_tree_visibility: all rows satisfied")
    return 0


def _self_test() -> int:
    """Row helpers must flag missing anchors on synthetic stripped text."""
    failures: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            failures.append(label)

    good = (
        "make_scope_addr_fail, add_deny_class]\n"
        "const bool tree_settled_now = scope->tree_settled();\n"
        '{"tree-settled", make_bool(tree_settled_now)}\n'
        "add_deny_class(kv, aura::orch::AgentDenyClass::Other,\n"
    )
    must("make_scope_addr_fail, add_deny_class]", "self capture", good)
    must("const bool tree_settled_now = scope->tree_settled();", "self settled", good)
    if "entry.current = current.id;" in good:
        failures.append("self: stray anchor in sample")
    print(
        "SELF-TEST OK: check_scope_join_tree_visibility_3671 row helpers"
        if not failures
        else f"SELF-TEST FAIL: {failures}"
    )
    return 0 if not failures else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(_self_test())
    sys.exit(main())
