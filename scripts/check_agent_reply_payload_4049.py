#!/usr/bin/env python3
"""Issue #4049: agent-reply drops non-scalar payloads / charges process bucket.

Contract (one row per AC):
  AC1  orch:agent-reply gives non-scalar payloads the #2848 StableNodeRef
       recognition (stamp_stable_ref + handoff_ref, post-handoff
       stable-ref:id:gen body, wired sentinel); string/int/bool stay
       zero-cost; the legacy 2-arg agent_reply(corr_id, payload) call is
       gone (handle + held token now passed)
  AC2  handoff failure returns the existing structured status
       (handoff-required / export-stale), bumps the handoff-fail counter,
       stamps the Handoff deny class, and does NOT push
  AC3  agent_reply accepts the replying agent's handle + held token:
       stamp_mail_message_handoff_completed(msg, *held_token) so the reply
       rides the recv-gate contract, BP notes from->bp_scope_id, and the
       producer-throttle arm (enter on N consecutive BP, Ok heals) runs —
       reply is a second send, not a process-bucket event
  AC4  no handle + empty scope under production_defaults_active does NOT
       bump the process bucket — the event lands on the at-cap overflow
       observability gauge; Soft keeps the process bucket contract
  AC5  agent_ask's recv loop honors stale_handoff as handoff-required
       instead of continue-until-timeout
  AC6  handle resolution: AgentNameTable::find_by_fiber +
       AgentScope::find_by_fiber (current fiber's handle, both planes);
       ACs live in tests/orch/test_agent_ask_typed_corr.cpp (4049 markers);
       no tests/**/test_issue_4049.cpp; no docs/design/4049-*
  AC7  no AgentRegistry, no new query key; one pending-ask map
  AC8  build.py wires check_agent_reply_payload_4049 + root allowlist

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

    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    spawn = _read("src/orch/agent_spawn.h")
    table = _read("src/compiler/agent_name_table.h")
    scope = _read("src/orch/agent_scope.h")
    test = _read("tests/orch/test_agent_ask_typed_corr.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: reply prim recognizes StableNodeRef, zero-cost scalars ──────
    must("Issue #4049", "AC1 reply prim cites", prim)
    must("agent-reply-auto-handoff-wired", "AC1 wired sentinel", prim)
    must("ev.stamp_stable_ref(held)", "AC1 stamp_stable_ref", prim)
    must("ev.handoff_ref(std::move(held))", "AC1 handoff_ref", prim)
    must("stable-ref:", "AC1 post-handoff body", prim)
    must('payload = "payload";', "AC1 legacy fallback retained for non-stable", prim)
    # Legacy 2-arg call is gone — handle + held token are passed now.
    forbid("aura::orch::agent_reply(corr_id, payload);", "AC1 legacy 2-arg call", prim)
    must("agent_reply(corr_id, payload, /*reply_dest=*/nullptr", "AC1 new call shape", prim)

    # ── AC2: structured fail, no push on handoff failure ─────────────────
    must('"export-stale"', "AC2 export-stale status", prim)
    must('"handoff-required"', "AC2 handoff-required status", prim)
    must("agent_send_handoff_fail_total", "AC2 handoff-fail counter", prim)
    must("AgentDenyClass::Handoff", "AC2 Handoff deny class", prim)

    # ── AC3: agent_reply handle + held token + throttle arm ──────────────
    must("std::optional<std::uint64_t> held_token = std::nullopt) noexcept {", "AC3 held_token param", spawn)
    must("stamp_mail_message_handoff_completed(msg, *held_token)", "AC3 reply stamp", spawn)
    must("note_mailbox_bp_recent_event(from->bp_scope_id, from->id)", "AC3 replying-scope BP", spawn)
    must("from->consecutive_bp_count >= from->producer_bp_budget", "AC3 throttle enter arm", spawn)
    must("agent_producer_throttle_enter_total", "AC3 throttle enter counter", spawn)
    must("agent_producer_throttle_clear_total", "AC3 Ok-heals counter", spawn)

    # ── AC4: production empty-scope → overflow gauge, bucket stays clean ─
    must("process bucket stays clean", "AC4 no-bucket comment", spawn)
    # Format-robust: the reply BP arm lands on the overflow gauge next to
    # the production_defaults_active() guard inside agent_reply.
    must("} else if (production_defaults_active()) {", "AC4 production overflow arm", spawn)
    must("note_mailbox_bp_recent_event(std::string_view{}, 0);", "AC4 soft bucket", spawn)

    # ── AC5: agent_ask honors stale_handoff ───────────────────────────────
    must("handoff-required beats continue-until-timeout", "AC5 ask stale comment", spawn)
    # Format-robust: stale branch returns before the continue.
    sig5 = spawn.find("handoff-required beats continue-until-timeout")
    if sig5 < 0:
        fails.append("AC5: stale branch anchor missing")
    else:
        window = spawn[sig5 : sig5 + 400]
        must("if (stale_handoff)", "AC5 stale branch", window)
        must('"handoff-required";', "AC5 typed status", window)
        must("continue;", "AC5 continue retained after branch", window)

    # ── AC6: handle resolution + test host + no new files ────────────────
    must("find_by_fiber", "AC6 name-table fiber lookup", table)
    must("find_by_fiber", "AC6 scope fiber lookup", scope)
    must("find_by_fiber(f->id())", "AC6 prim resolves current-fiber handle", prim)
    must("Issue #4049", "AC6 test cites", test)
    must("#4049 AC1", "AC6 test AC1", test)
    must("#4049 AC5", "AC6 test AC5", test)
    must("handoff-required", "AC6 typed status in test", test)
    if _read("tests/orch/test_issue_4049.cpp"):
        fails.append("AC6: tests/orch/test_issue_4049.cpp must not exist")
    if _read("tests/issues/test_issue_4049.cpp"):
        fails.append("AC6: tests/issues/test_issue_4049.cpp must not exist")
    if _read("docs/design/4049-agent-reply-payload.md"):
        fails.append("AC6: docs/design/4049 file must not exist")

    # ── AC7: no AgentRegistry / no new query key ─────────────────────────
    forbid("class AgentRegistry", "AC7 no AgentRegistry", spawn)
    forbid("query:agent-reply", "AC7 no new query key", prim)

    # ── AC8: build.py wiring + root allowlist ─────────────────────────────
    must("check_agent_reply_payload_4049", "AC8 build.py wires linter", build)
    must("check_agent_reply_payload_4049.py", "AC8 root allowlist", allow)

    if fails:
        for f in fails:
            print(f"4049 FAIL: {f}", file=sys.stderr)
        return 1
    print("4049 OK: agent-reply payload/BP/stale contracts satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
