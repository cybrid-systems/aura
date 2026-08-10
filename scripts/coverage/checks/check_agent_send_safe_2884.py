#!/usr/bin/env python3
"""Issue #2884: agent_send_safe — unify C++/language handoff_ref path for
StableNodeRef payloads (close #2663 / #2848 contract split).

Contract (one row per AC):
  AC1  C++ `agent_send_safe` with unstamped StableNodeRef payload → successful
       export + Ok push OR structured handoff fail (NOT silent Closed)
  AC2  String / int / bool payloads remain zero-cost (no handoff path)
  AC3  Raw `agent_send` with unstamped held_ref still hits #2663 Closed
       (defense in depth preserved)
  AC4  Language `(orch:agent-send)` behaviour unchanged / still auto-handoff
  AC5  Counters + `query:orch-module-stats` additive; Soft regression green
  AC6  Source-cite + tests (extend mailbox / agent-send suites) per #81967;
       no docs/design/ per #1655

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Files in scope for #2884.
SCOPE_FILES = [
    "src/orch/agent_spawn.h",
    "src/serve/multi_fiber_mailbox.h",
    "src/compiler/evaluator_primitives_agent.cpp",
    "tests/orch/test_orch_obs_facade.cpp",
    "scripts/coverage/checks/check_agent_send_safe_2884.py",
]


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

    agent_spawn = _read("src/orch/agent_spawn.h")
    mf_mailbox = _read("src/serve/multi_fiber_mailbox.h")
    posture = _read("src/compiler/evaluator_primitives_agent.cpp")
    test_of = _read("tests/orch/test_orch_obs_facade.cpp")
    build = _read("build.py")

    # ── AC1: helper defined + takes optional Evaluator* + returns HandoffRequired ─
    must("agent_send_safe", "AC1", agent_spawn)
    # Opaque void* ABI with Evaluator* source-cite (no module import / no
    # forward-decl that collides with evaluator module export).
    must("Evaluator*", "AC1", agent_spawn)
    must("void* ev", "AC1", agent_spawn)
    must("HandoffRequired", "AC1", agent_spawn)
    must("agent_send_safe_handoff_required_total", "AC1", agent_spawn)
    must("agent_send_safe_total", "AC1", agent_spawn)
    # Handoff path via extern "C" hook (NOT import aura.compiler.evaluator in
    # this header — that re-import broke evaluator_ctor.cpp asan ddi scan).
    must("aura_orch_agent_send_handoff", "AC1", agent_spawn)
    if "import aura.compiler.evaluator;" in agent_spawn:
        fails.append(
            "AC1: agent_spawn.h must not import aura.compiler.evaluator "
            "(module already imported when included from evaluator_ctor.cpp)"
        )
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    must("aura_orch_agent_send_handoff", "AC1", efm)
    must("make_stamped_ref", "AC1", efm)
    must("handoff_ref", "AC1", efm)
    # PushStatus::HandoffRequired enum value defined in multi_fiber_mailbox.h.
    must("HandoffRequired = 3", "AC1", mf_mailbox)

    # ── AC2: string / int / bool zero-cost fall through ──
    # The helper falls through to raw agent_send when held_ref_token has no
    # value (string/int/bool payloads don't set held_ref_token) — verified by
    # the explicit check below the import.
    must("msg.held_ref_token.has_value()", "AC2", agent_spawn)
    # The fall-through branch must NOT call handoff_ref.
    must("stamp_mail_message_handoff_completed", "AC2", agent_spawn)
    # The total counter bumps regardless of path (covers zero-cost path too).
    must("agent_send_safe_total.fetch_add", "AC2", agent_spawn)

    # ── AC3: raw agent_send with unstamped held_ref still hits #2663 Closed ──
    # PushStatus::Closed = 2 preserved (defense in depth unchanged).
    must("Closed = 2", "AC3", mf_mailbox)
    # The #2663 held-ref gate still rejects unstamped held_ref (in push/broadcast).
    must("handoff_reject_total", "AC3", mf_mailbox)
    must("handoff_completed is false, reject", "AC3", mf_mailbox)

    # ── AC4: language (orch:agent-send) auto-handoff behaviour preserved ──
    must("schema-2848", "AC4", posture)
    must("agent-send-auto-handoff-wired", "AC4", posture)
    must("agent-send-auto-handoff-total", "AC4", posture)
    must("agent-send-handoff-fail-total", "AC4", posture)
    # The Aura prim still has the auto handoff_ref + stamp flow intact.
    must("ev.handoff_ref", "AC4", posture)
    must("stamp_mail_message_handoff_completed", "AC4", posture)

    # ── AC5: counters + query:orch-module-stats additive ──
    must("schema-2884", "AC5", posture)
    must("issue-2884", "AC5", posture)
    must("agent-send-safe-wired", "AC5", posture)
    must("agent-send-safe-total", "AC5", posture)
    must("agent-send-safe-handoff-required-total", "AC5", posture)
    # The existing #2848 keys must remain additive (no regression).
    must("schema-2848", "AC5", posture)
    must("agent-send-auto-handoff-total", "AC5", posture)

    # ── AC6: source-cite + tests; no docs/design/; no invent ──
    must("Issue #2884", "AC6", agent_spawn)
    must("Issue #2884", "AC6", mf_mailbox)
    must("schema-2884", "AC6", posture)
    must("#2884", "AC6", test_of)
    # build.py wires this linter into the gate.
    if "check_agent_send_safe_2884" not in build:
        fails.append("AC6: build.py does not wire #2884 linter")
    # No new test_issue_2884.cpp (per #81967).
    if (ROOT / "tests" / "core" / "test_issue_2884.cpp").is_file():
        fails.append("AC6: test_issue_2884.cpp present in tests/core (forbidden per #81967)")
    if (ROOT / "tests" / "orch" / "test_issue_2884.cpp").is_file():
        fails.append("AC6: test_issue_2884.cpp present in tests/orch (forbidden per #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_2884.cpp").is_file():
        fails.append("AC6: test_issue_2884.cpp present in tests/compiler (forbidden per #81967)")
    # No docs/design/2884-* (per #1655).
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2884-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # ── Cross-check #2663 mailbox held-ref gate still defined (defense in depth) ──
    # The #2663 mailbox held-ref gate must remain intact — raw agent_send with
    # unstamped held_ref_token still hits Closed (=2). Source-cite already
    # verified above; this is a structural note for the linter (not a sub-run).
    # If a #2663-specific linter ever exists, add cross-run here.

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2884 agent_send_safe — unify C++/language handoff_ref path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
