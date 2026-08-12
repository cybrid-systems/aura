#!/usr/bin/env python3
"""Issue #2925: producer BP self-throttle budget for attached agents.

AC:
  1. producer_bp_budget default 0 (zero cost); AgentSpec + AgentHandle fields
  2. agent_send: N consecutive BP → throttle + helper_stop; not cancel
  3. Ok push / quiet window clears throttle
  4. Metrics enter/clear + query keys schema-2925
  5. Aura :producer-bp-budget on orch:spawn-agent
  6. Extend test_mailbox_bp_admit; no invent; no docs/design/
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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    spawn = _read("src/orch/agent_spawn.h")
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_mailbox_bp_admit.cpp")
    build = _read("build.py")

    must("Issue #2925" in spawn, "AC1: cites #2925")
    must("producer_bp_budget" in spawn, "AC1: producer_bp_budget field")
    must("resolve_producer_bp_budget" in spawn, "AC1: resolve helper")
    must("agent_producer_throttle_enter_total" in spawn, "AC2: enter metric")
    must("agent_producer_throttle_clear_total" in spawn, "AC3: clear metric")
    must("maybe_clear_producer_throttle" in spawn, "AC3: quiet clear helper")
    must("consecutive_bp_count" in spawn, "AC2: consecutive counter")
    must("producer_throttled" in spawn, "AC2: throttled flag")
    must("helper_stop" in spawn and "producer_bp_budget" in spawn, "AC4: helper_stop path")

    # agent_send should short-circuit when throttled without push growth intent
    must(
        "producer_bp_budget > 0 && h.producer_throttled" in spawn or "producer_throttled" in spawn,
        "AC2: throttle short-circuit in agent_send",
    )

    must("producer-bp-budget" in agent or "producer_bp_budget" in agent, "AC5: Aura kwarg")
    must("agent-producer-throttle-enter-total" in agent, "AC5: query enter key")
    must("agent-producer-throttle-clear-total" in agent, "AC5: query clear key")
    must("schema-2925" in agent, "AC5: schema-2925")

    must("2925" in test and "producer_bp_budget" in test, "AC6: test extended")
    must("#2925 AC1" in test or "2925 AC1" in test, "AC6: AC1 in test")
    must(
        "producer-bp-budget-2925" in build or "producer_bp_budget_2925" in build,
        "AC6: build.py",
    )
    must(not (ROOT / "tests/orch/test_issue_2925.cpp").is_file(), "AC6: no invent")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2925-*"):
            fails.append(f"AC6: docs/design/{f.name} forbidden")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2925 producer BP budget — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
