#!/usr/bin/env python3
"""Issue #2972: per-mailbox inflight credit / push backpressure.

AC:
  1. inflight == credit → push Backpressure; recv decrements → next push Ok
  2. close / dtor drops queued + zeros inflight (no permanent credit poison)
  3. process/scope recent admit (#2535) stays independent; credit BP notes recent
  4. #2925 producer throttle still keys off consecutive BP (credit BP counts)
  5. additive metrics + extend test_mailbox_bp_admit; no invent; no docs/design/
  6. MVP scope: no AgentRegistry
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mb = _read("src/serve/multi_fiber_mailbox.h")
    spawn = _read("src/orch/agent_spawn.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    test = _read("tests/orch/test_mailbox_bp_admit.cpp")
    build = _read("build.py")

    must("Issue #2972", "AC1 mailbox", mb)
    must("kMailboxCreditInflightIssue = 2972", "AC1 stamp", mb)
    must("inflight_", "AC1 inflight field", mb)
    must("credit_limit_", "AC1 credit_limit", mb)
    must("effective_credit", "AC1 effective_credit", mb)
    must("note_credit_backpressure", "AC1 credit BP helper", mb)
    must("add_inflight_", "AC1 increment", mb)
    must("dec_inflight_", "AC1 decrement", mb)
    must("drop_queued_unlocked_", "AC2 close drain", mb)

    # Pre-move / credit gate before high_water enqueue.
    cred_pos = mb.find("inflight_.load(std::memory_order_relaxed) >= effective_credit()")
    hw_pos = mb.find("queue_.size() >= high_water_")
    if cred_pos == -1 or hw_pos == -1 or cred_pos > hw_pos:
        fails.append("AC1: credit gate must precede high_water check in push")

    must("mailbox_credit", "AC1 AgentSpec", spawn)
    must("spec.mailbox_credit", "AC1 spawn wires credit", spawn)
    must("mailbox-credit", "AC5 Aura kwarg", agent)

    must("mailbox-credit-bp-total", "AC5 orch-module-stats", agent)
    must("mailbox-inflight-hwm", "AC5 orch-module-stats", agent)
    must("schema-2972", "AC5 orch-module-stats", agent)
    must("mailbox-credit-wired", "AC5 orch-module-stats", agent)
    must("schema-2972", "AC5 mf-mailbox-stats", msg)
    must("mailbox-credit-bp-total", "AC5 mf-mailbox-stats", msg)

    must("note_backpressure", "AC3 still notes recent via existing helper", mb)
    must("bp_recent >= threshold", "AC3 admit independent", spawn)
    must("consecutive_bp_count", "AC4 producer throttle retained", spawn)

    must("2972 AC1", "AC5 test AC1", test)
    must("2972 AC2", "AC5 test AC2", test)
    must("2972 AC3", "AC5 test AC3", test)
    must("2972 AC4", "AC5 test AC4", test)
    must("2972 AC5", "AC5 test AC5", test)
    must("2972 AC6", "AC5 test AC6", test)

    must("mailbox-credit-2972", "AC5 build.py", build)
    must("cmd_mailbox_credit_inflight_2972", "AC5 build cmd", build)

    if "AgentRegistry" in mb:
        fails.append("AC6: multi_fiber_mailbox.h mentions AgentRegistry")
    must("AgentRegistry", "AC6 test cites ban", test)

    if (ROOT / "tests" / "orch" / "test_issue_2972.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_2972.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2972-*"):
            fails.append(f"AC5: docs/design/{f.name} forbidden (#1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_mailbox_credit_inflight_2972: {len(fails)} failure(s)")
        return 1
    print("check_mailbox_credit_inflight_2972: OK (AC1-AC6)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
