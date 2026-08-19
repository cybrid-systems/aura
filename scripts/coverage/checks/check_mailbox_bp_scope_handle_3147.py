#!/usr/bin/env python3
"""Issue #3147: agent_send / emit_keepalive BP events route to handle's
effective bp_scope_id, so a scoped AgentScope's BP storm does not poison
the process bucket.

Contract (one row per AC):
  AC1  Production AgentScope spawn inherit → subsequent agent_send BP
       bumps the scope gauge only (process mailbox_bp_recent_total
       unchanged for that event). Closes the runtime cross-poison
       window that #3015 left open at admit.
  AC2  Explicit bp_scope_id = "-" or empty on direct spawn_agent_with_mailbox
       → process bucket (Soft / single-agent MVP / explicit opt-in;
       zero behavioural change).
  AC3  Keepalive helper fiber (emit_keepalive) BP under a scoped agent
       routes to the same scope gauge as body sends — mirrors agent_send.
  AC4  Spawn admit + watch_all on_backpressure still use SSOT
       load_mailbox_bp_recent (no dual threshold logic).
  AC5  Soft / sandbox=off: empty scope remains zero extra atomic beyond
       existing note path (no auto-fill, process bucket unchanged).
  AC6  Additive counters only if needed (prefer existing scope / process
       reject totals); no new query key; no metrics middle insertion.
  AC7  Extend tests/orch/test_per_scope_bp_admit.cpp (or test_mailbox_bp_admit.cpp)
       with agent_send BP storm isolation across two scopes. No
       test_issue_3147.cpp per #81967; no docs/design/3147-* per #1655.
  AC8  Source-cite + coverage linter wired into build.py; no
       process-global AgentRegistry.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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
    scope = _read("src/orch/agent_scope.h")
    test = _read("tests/orch/test_per_scope_bp_admit.cpp")
    build = _read("build.py")

    # ── AC1: agent_send BP arm routes to handle's bp_scope_id
    # Issue #3147 marker must appear in agent_spawn.h.
    must("Issue #3147", "AC1 source-cite marker in agent_spawn.h", spawn)
    # AgentHandle gains a bp_scope_id field (layout-stable end of struct).
    must("std::string bp_scope_id{};", "AC1 AgentHandle::bp_scope_id field present", spawn)
    # Spawn propagates spec.bp_scope_id → handle.bp_scope_id.
    must("h.bp_scope_id = std::move(spec.bp_scope_id);", "AC1 spawn propagates spec.bp_scope_id → handle", spawn)
    # agent_send BP arm passes h.bp_scope_id (instead of empty default).
    # The exact text is searched twice (agent_send + emit_keepalive).
    must(
        "note_mailbox_bp_recent_event(h.bp_scope_id)",
        "AC1 agent_send BP arm passes h.bp_scope_id (not empty default)",
        spawn,
    )
    # note_mailbox_bp_recent_event helper still routes empty scope_id
    # to process bucket (Soft / MVP unchanged).
    must(
        "void note_mailbox_bp_recent_event(std::string_view scope_id",
        "AC1 note_mailbox_bp_recent_event helper accepts scope_id parameter",
        spawn,
    )

    # ── AC2: explicit "-" / empty → process bucket (existing Soft / MVP path)
    # kBpScopeProcessBucket sentinel must exist (existing #3015 surface).
    must("kBpScopeProcessBucket", "AC2 '-' sentinel still wired (#3015)", spawn)
    # helper short-circuits empty scope_id to process bucket (no scope lookup).
    if "scope_id.empty()" not in spawn:
        fails.append("AC2: empty scope_id short-circuit to process bucket missing")
    # note_mailbox_bp_recent_event increments mailbox_bp_recent_total on
    # empty scope_id (process bucket path).
    if "mailbox_bp_recent_total.fetch_add" not in spawn:
        fails.append("AC2: mailbox_bp_recent_total fetch_add missing (process bucket path)")

    # ── AC3: emit_keepalive BP arm routes to same scope gauge
    # emit_keepalive helper must exist and its BP arm must also pass
    # h.bp_scope_id (mirror of agent_send). The helper sits inside the
    # keepalive / liveness path; find the definition and verify the BP
    # arm text. The actual return type is serve::mf_mailbox::PushStatus
    # (not void — it returns the push status to the caller).
    emit_idx = spawn.find("emit_keepalive(")
    if emit_idx < 0:
        fails.append("AC3: emit_keepalive helper missing")
    else:
        snip = spawn[emit_idx : emit_idx + 4000]
        if "note_mailbox_bp_recent_event(h.bp_scope_id)" not in snip:
            fails.append("AC3: emit_keepalive BP arm must pass h.bp_scope_id (mirror of agent_send)")

    # ── AC4: spawn admit + watch_all on_backpressure use SSOT
    # load_mailbox_bp_recent helper must still be wired in spawn admit
    # (agent_spawn.h) and AgentScope admit preflight (agent_scope.h).
    must("load_mailbox_bp_recent(", "AC4 load_mailbox_bp_recent helper wired in spawn", spawn)
    must("load_mailbox_bp_recent(", "AC4 load_mailbox_bp_recent helper wired in AgentScope", scope)

    # ── AC5: Soft / empty unchanged
    # production_scope_bp_inherit() is the production gate (#3015); Soft
    # path leaves spec.bp_scope_id empty (no auto-fill).
    must("production_scope_bp_inherit", "AC5 production gate #3015 preserved", scope)
    # Helper short-circuits empty scope_id (already verified in AC2).

    # ── AC6: additive counters only (no new query key, no metrics middle)
    # The fix must not insert a new metric key in evaluator_primitives_security.cpp
    # or anywhere else. Source-cite: bp_scope_id is a string field, not a
    # counter, and the existing surface (mailbox_bp_recent_total +
    # scope gauges) covers the contract. Forbidden: any new atomic / counter
    # named after #3147.
    agent_prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    for forbidden in (
        "schema-3147",
        "issue-3147",
        "agent_send_scope_bp_total",
        "agent_keepalive_scope_bp_total",
        "handle_bp_scope_total",
    ):
        for hay in (spawn, scope, agent_prim):
            if forbidden in hay:
                fails.append(f"AC6: forbidden new metric key {forbidden!r}")
    # No new insert_kv or query:security-posture entry for #3147.
    if "schema-3147" in _read("src/compiler/evaluator_primitives_security.cpp"):
        fails.append("AC6: schema-3147 leaked into security posture (forbidden)")

    # ── AC7: tests extend existing; no test_issue_3147.cpp + no docs/design/3147-*
    must("#3147", "AC7 test file cites Issue #3147", test)
    must("3147 AC1", "AC7 test file marks #3147 AC1", test)
    must("3147 AC2", "AC7 test file marks #3147 AC2", test)
    must("3147 AC3", "AC7 test file marks #3147 AC3", test)
    must("3147 AC4", "AC7 test file marks #3147 AC4", test)
    must("3147 AC5", "AC7 test file marks #3147 AC5", test)
    if (ROOT / "tests" / "orch" / "test_issue_3147.cpp").is_file():
        fails.append("AC7: tests/orch/test_issue_3147.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3147.cpp").is_file():
        fails.append("AC7: tests/core/test_issue_3147.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3147-*")):
            fails.append(f"AC7: docs/design/{f.name} present (forbidden #1655)")

    # ── AC8: source-cite + linter wired + no process-global AgentRegistry
    must("Issue #3147", "AC8 agent_spawn.h cites Issue #3147", spawn)
    # Spawn admit + AgentScope + note_mailbox_bp_recent helper all
    # preserve the existing surface (no breakage in the source-cite chain).
    must("#3015", "AC8 lineage preserved (AgentScope #3015 inherit)", scope)
    # Linter wired into build.py.
    must("check_mailbox_bp_scope_handle_3147", "AC8 build.py wiring for #3147 linter", build)
    must("Issue #3147", "AC8 build.py linter error message references #3147", build)
    # No process-global AgentRegistry. Strip comments before checking —
    # the file-header warning lists the #1966 forbidden patterns.
    spawn_no_comments = re.sub(r"//[^\n]*", "", spawn)
    spawn_no_comments = re.sub(r"/\*.*?\*/", "", spawn_no_comments, flags=re.DOTALL)
    if re.search(r"\bglobal_agent_registry\b|\bprocess_agent_registry\b", spawn_no_comments):
        fails.append("AC8: process-global AgentRegistry leaked into agent_spawn.h")
    # Soft path is unchanged: production_scope_bp_inherit() gate preserves
    # Soft zero-cost (no auto-fill under sandbox=off / Soft).
    if "production_scope_bp_inherit" not in scope:
        fails.append("AC8: production_scope_bp_inherit gate missing (#3015 / Soft AC5)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3147 agent_send/emit_keepalive BP routes to handle bp_scope_id — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
