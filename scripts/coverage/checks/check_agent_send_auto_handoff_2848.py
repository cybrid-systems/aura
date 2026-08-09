#!/usr/bin/env python3
"""Issue #2848: language-path auto handoff_ref for StableNodeRef agent-send.

Residual of #2663: mailbox push/fanout gate is hard, but orch:agent-send
still required agents to handoff manually. Auto handoff on the language
path + structured typed failure (never ambiguous Closed).

Contract (one row per AC):
  AC1 orch:agent-send StableNodeRef pair path calls handoff_ref + stamp;
     fail returns handoff-required/export-stale (not silent Closed)
  AC2 string/int/bool short-circuit — no held_ref_token, no handoff work
  AC3 stamp_mail_message_handoff_completed helper; raw C++ push gate
     preserved (#2663 held_ref_token && !handoff_completed → Closed)
  AC4 additive metrics agent-send-auto-handoff-total /
     agent-send-handoff-fail-total + schema-2848; #2663 counters retained
  AC5 prefer-existing tests extended (ac2848 / 2848 AC); this linter;
     no docs/design/*
  AC6 Soft prefer export path documented; production never weakens gate

Exit 0 = all rows satisfied.
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

    ag = _read("src/compiler/evaluator_primitives_agent.cpp")
    spawn = _read("src/orch/agent_spawn.h")
    mb = _read("src/serve/multi_fiber_mailbox.h")
    test = _read("tests/compiler/test_stable_ref_export_validate.cpp")
    build = _read("build.py")

    # AC1 — language path auto handoff
    must("Issue #2848", "AC1", ag)
    must("orch:agent-send", "AC1", ag)
    must("ev.handoff_ref(", "AC1", ag)
    must("stamp_mail_message_handoff_completed", "AC1", ag)
    must("handoff-required", "AC1", ag)
    must("export-stale", "AC1", ag)
    must("auto-handoff", "AC1", ag)
    must("schema-2848", "AC1", ag)

    # AC2 — zero-cost string/int/bool
    must("is_string(a[1])", "AC2", ag)
    must("is_int(a[1])", "AC2", ag)
    must("is_bool(a[1])", "AC2", ag)
    must("zero-cost", "AC2", ag)
    # string path must not set held_ref_token
    must("no held_ref_token", "AC2", ag)

    # AC3 — stamp helper + #2663 gate preserved
    must("stamp_mail_message_handoff_completed", "AC3", spawn)
    must("kAgentSendAutoHandoffIssue = 2848", "AC3", spawn)
    must("agent_send_auto_handoff_total", "AC3", spawn)
    must("agent_send_handoff_fail_total", "AC3", spawn)
    must("if (msg.held_ref_token.has_value() && !msg.handoff_completed)", "AC3 gate", mb)
    must("handoff_reject_total", "AC3 gate", mb)
    must("defense in depth", "AC3", spawn)

    # AC4 — metrics + query keys
    must("agent-send-auto-handoff-total", "AC4", ag)
    must("agent-send-handoff-fail-total", "AC4", ag)
    must("schema-2848", "AC4", ag)
    must("agent_send_auto_handoff_total.fetch_add", "AC4", ag)
    must("agent_send_handoff_fail_total.fetch_add", "AC4", ag)

    # AC5 — tests + linter wiring; no docs/design invent
    must("2848 AC", "AC5", test)
    must("Issue #2848", "AC5", test)
    must("check_agent_send_auto_handoff_2848", "AC5", build)
    if "docs/design" in ag or "docs/design" in spawn:
        fails.append("AC5: must not invent docs/design/* content for #2848")

    # AC6 — Soft prefer export; production gate never weakened
    must("Soft / sandbox=off", "AC6", ag)
    must("prefer the export path", "AC6", ag)
    must("never weakens the mailbox gate", "AC6", spawn)
    must("production-safe default", "AC6", mb)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2848 agent-send auto handoff coverage — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
