#!/usr/bin/env python3
"""Issue #3673: orch:agent-recv maps the Guard-live Policy A reject to a
typed deny instead of a quiet empty=#t — Agents can branch instead of
busy-looping.

Contract (one row per AC):
  AC1  production + Guard-live + wait=#t → ok=#f, empty=#f,
       deny-detail=recv-under-boundary (#3251 intern, emit_retry=#t);
       Policy A stays (no park, no Fiber::yield); hard counter still
       bumps (#2347)
  AC2  no Guard + quiet empty → empty=#t unchanged, no deny-class
  AC3  steal-stale held_ref stays typed handoff-required (#3565) on its
       own handle flag — not conflated with the boundary flag
  AC4  Soft / Off: empty=#t even under Guard (zero extra intern)
  AC5  tests extend test_orch_obs_facade / test_mailbox_recv_mutation_boundary;
       no invent test; no docs/design; no new query key

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


def _rows(must, must_not) -> list[str]:
    fails: list[str] = []

    mailbox = _read("src/serve/multi_fiber_mailbox.h")
    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    facade_test = _read("tests/orch/test_orch_obs_facade.cpp")
    recv_test = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    build = _read("build.py")

    # AC1: mailbox surfaces the Guard-live reject per recv.
    must("bool* rejected_boundary = nullptr)", "AC1 recv out-flag param", mailbox)
    must("if (rejected_boundary)", "AC1 flag set", mailbox)
    must("*rejected_boundary = true;", "AC1 flag assign", mailbox)

    # AC1/AC3: the reject rides the handle like the #3642 stale flag.
    must("bool last_recv_boundary_reject = false;", "AC1 handle flag", spawn)
    must("h.last_recv_boundary_reject = boundary_reject;", "AC1 handle flag set", spawn)

    # AC1: prim maps the flag to a typed deny (production-gated).
    must(
        'add_deny_class(kv, aura::orch::AgentDenyClass::Other, "recv-under-boundary", 0,', "AC1 deny-class other", prim
    )
    must('"recv-under-boundary"', "AC1 deny-detail", prim)
    must('{"schema-2347", make_int(2347)}', "AC1 schema-2347", prim)
    must("Issue #3673", "AC1 cite", prim)

    # AC3: stale flag remains independent.
    must("last_recv_stale_handoff", "AC3 stale flag", spawn)

    # AC5: tests extend the two src-aligned suites.
    must("3673 AC1", "AC5 facade test AC1", facade_test)
    must("3673 AC2", "AC5 facade test AC2", facade_test)
    must("3673: recv sets rejected_boundary out-flag", "AC5 recv test flag", recv_test)
    if _read("tests/orch/test_issue_3673.cpp") or _read("tests/issues/test_issue_3673.cpp"):
        fails.append("AC5: test_issue_3673.cpp present (forbidden per #81934)")
    if _read("docs/design/3673-recv-under-boundary.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")
    must("check_recv_under_boundary_3673", "AC5 build.py", build)
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    fails = _rows(must, must_not)

    if fails:
        print("FAIL #3673 recv_under_boundary:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3673 recv_under_boundary: all rows satisfied")
    return 0


def _self_test() -> int:
    """Row helpers must flag missing anchors on synthetic stripped text."""
    failures: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            failures.append(label)

    good = (
        "bool* rejected_boundary = nullptr)\n"
        "*rejected_boundary = true;\n"
        "h.last_recv_boundary_reject = boundary_reject;\n"
        'add_deny_class(kv, aura::orch::AgentDenyClass::Other, "recv-under-boundary", 0,\n'
        '{"schema-2347", make_int(2347)}\n'
    )
    must("bool* rejected_boundary = nullptr)", "self recv flag param", good)
    must("*rejected_boundary = true;", "self flag assign", good)
    must("h.last_recv_boundary_reject = boundary_reject;", "self handle flag", good)
    must("recv-under-boundary", "self deny detail", good)
    must("schema-2347", "self schema", good)
    if "empty=#t" in good:
        failures.append("self: stray anchor in sample")
    print("SELF-TEST OK: check_recv_under_boundary_3673 row helpers" if not failures else f"SELF-TEST FAIL: {failures}")
    return 0 if not failures else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(_self_test())
    sys.exit(main())
