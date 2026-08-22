#!/usr/bin/env python3
"""Issue #3212: mailbox->push / agent_send dual-track HandoffRequired.

#3013 / #2884 made agent_send return HandoffRequired for unstamped
held_ref. Direct MultiFiberMailbox::push still returned Closed.
This residual aligns the typed fail.

Contract (one row per AC):
  AC1  unstamped held_ref → HandoffRequired from agent_send AND
       mailbox->push / broadcast_fanout. Still bumps handoff_reject_total.
  AC2  true closed mailbox still Closed. linear-viol still Closed.
  AC3  Soft / no held_ref: zero extra (optional has_value short-circuit).
  AC4  existing #2663 / #3013 / #3111 gates remain (reject, not enqueue).
  AC5  schema-3212 additive; schema-3013 preserved.
  AC6  tests extend test_orch_obs_facade; linter wired; no invent.

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

    mb = _read("src/serve/multi_fiber_mailbox.h")
    spawn = _read("src/orch/agent_spawn.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_orch_obs_facade.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3212", "AC1 mailbox cite", mb)
    must("return PushStatus::HandoffRequired", "AC1 push/fanout status", mb)
    must("if (msg.held_ref_token.has_value() && !msg.handoff_completed)", "AC1 push gate", mb)
    must("if (proto.held_ref_token.has_value() && !proto.handoff_completed)", "AC1 fanout gate", mb)
    must("HandoffRequired", "AC1 agent_send", spawn)
    must("3212 AC1", "AC1 test", test)

    # Push gate window returns HandoffRequired (not Closed)
    p = mb.find("if (msg.held_ref_token.has_value() && !msg.handoff_completed)")
    if p < 0:
        fails.append("AC1: push held_ref gate missing")
    else:
        win = mb[p : p + 450]
        if "return PushStatus::HandoffRequired" not in win:
            fails.append("AC1: push gate must return HandoffRequired")
        if "handoff_reject_total.fetch_add" not in win:
            fails.append("AC1: push gate must still bump handoff_reject_total")
    fp = mb.find("if (proto.held_ref_token.has_value() && !proto.handoff_completed)")
    if fp < 0:
        fails.append("AC1: fanout held_ref gate missing")
    else:
        win = mb[fp : fp + 450]
        if "return PushStatus::HandoffRequired" not in win:
            fails.append("AC1: fanout gate must return HandoffRequired")

    # AC2
    must("if (closed_.load(std::memory_order_relaxed))", "AC2 closed check", mb)
    must("reject_if_linear_viol", "AC2 linear-viol", mb)
    must("3212 AC2", "AC2 test", test)

    # AC3
    must("Zero cost when", "AC3 zero cost", mb)
    must("3212 AC3", "AC3 test", test)

    # AC4
    must("defense in depth", "AC4 mailbox gate kept", spawn)
    must("handoff_reject_total", "AC4 reject counter", mb)

    # AC5
    must("schema-3212", "AC5 schema", agent)
    must("mailbox-handoff-required-wired", "AC5 wired", agent)
    must("schema-3013", "AC5 3013 preserved", agent)
    must("3212 AC5", "AC5 test", test)

    # AC6
    must("check_mailbox_handoff_dual_track_3212", "AC6 build", build)
    must("3212 AC6", "AC6 test", test)
    if _read("docs/design/3212-mailbox-handoff-dual-track.md"):
        fails.append("AC6: docs/design/3212-* present (forbidden per #1655)")
    if (ROOT / "tests" / "orch" / "test_issue_3212.cpp").is_file():
        fails.append("AC6: tests/orch/test_issue_3212.cpp present (forbidden per #81967)")

    if fails:
        print(f"Issue #3212 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3212 mailbox/agent_send HandoffRequired dual-track — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
