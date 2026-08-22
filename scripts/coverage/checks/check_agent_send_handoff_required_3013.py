#!/usr/bin/env python3
"""Issue #3013: raw agent_send unstamped held_ref → HandoffRequired.

Contract (one row per AC):
  AC1  Raw agent_send with held_ref_token set and handoff_completed=false
       returns PushStatus::HandoffRequired (not Closed). Same typed fail
       as agent_send_safe. Reuses agent_send_safe_handoff_required_total.
  AC2  Zero-cost fall-through when no held_ref_token / already stamped
       (no extra atomic on the ordinary string path beyond the same
       optional+bool the mailbox gate would pay).
  AC3  Direct mailbox->push returns HandoffRequired for unstamped held_ref
       (#3212 dual-track align; #2663 gate still rejects). Truly closed
       mailboxes still Closed.
  AC4  New C++ sites directed to agent_send_safe (deprecation / prefer
       comment). No second orch model / no AgentRegistry.
  AC5  Additive schema-3013 on query:orch-module-stats; existing
       agent_send_safe / mailbox BP tests stay green.
  AC6  Extend test_orch_obs_facade (#81967); no test_issue_3013.cpp;
       no docs/design/ (#1655).

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

    spawn = _read("src/orch/agent_spawn.h")
    mb = _read("src/serve/multi_fiber_mailbox.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_orch_obs_facade.cpp")
    build = _read("build.py")

    # ── AC1: agent_send returns HandoffRequired before push ────────
    must("Issue #3013", "AC1", spawn)
    must("HandoffRequired", "AC1", spawn)
    send = spawn.find("Issue #3013: unstamped held_ref is a typed handoff miss")
    if send < 0:
        fails.append("AC1: agent_send #3013 pre-push gate not found")
    else:
        body = spawn[send : send + 1200]
        if "HandoffRequired" not in body:
            fails.append("AC1: agent_send body must return HandoffRequired")
        if "held_ref_token.has_value()" not in body:
            fails.append("AC1: agent_send must check held_ref_token before push")
        push_p = spawn.find("mailbox->push", send)
        hr_p = spawn.find("HandoffRequired", send)
        if push_p < 0 or hr_p < 0 or hr_p > push_p:
            fails.append("AC1: HandoffRequired return must precede mailbox->push")

    # ── AC2: zero-cost no-token / stamped ─────────────────────────
    must("Zero-cost when no", "AC2", spawn)
    must("handoff_completed", "AC2", spawn)

    # ── AC3: mailbox push unstamped HandoffRequired; true closed Closed ─
    must("return PushStatus::HandoffRequired", "AC3", mb)
    must("return PushStatus::Closed", "AC3 true-closed preserved", mb)
    must("handoff_reject_total", "AC3", mb)
    must("if (msg.held_ref_token.has_value() && !msg.handoff_completed)", "AC3", mb)

    # ── AC4: prefer agent_send_safe ───────────────────────────────
    must("prefer agent_send_safe", "AC4", spawn)
    if "AgentRegistry" in spawn[spawn.find("Issue #3013") : spawn.find("Issue #3013") + 600]:
        fails.append("AC4: #3013 must not introduce AgentRegistry")

    # ── AC5: additive schema, reuse counters ──────────────────────
    must("schema-3013", "AC5", agent)
    must("agent-send-handoff-required-wired", "AC5", agent)
    must("agent_send_safe_handoff_required_total", "AC5", spawn)
    must("schema-2884", "AC5", agent)

    # ── AC6: tests + no invent + no docs/design/ ──────────────────
    must("#3013 AC1", "AC6", test)
    must("check_agent_send_handoff_required_3013", "AC6", build)
    for rel in (
        "tests/orch/test_issue_3013.cpp",
        "tests/compiler/test_issue_3013.cpp",
        "tests/core/test_issue_3013.cpp",
    ):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #81967")
    for rel in (
        "docs/design/3013-agent-send-handoff-required.md",
        "docs/design/3013-*.md",
    ):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #3013 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3013 agent_send HandoffRequired — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
