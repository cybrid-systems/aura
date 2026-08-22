#!/usr/bin/env python3
"""Issue #3014: surface agent body try_acquire reject on handle / join hash.

Contract (one row per AC):
  AC1  On aura_orch_agent_body_try_acquire_ex reject, fiber stores
       AgentHandle.body_acquire_rejected (shared_ptr<atomic<bool>>)
       before exit. JoinStatus stays Ok (fiber completed).
  AC2  Aura orch:agent-join hash keys (body-acquire-rejected,
       schema-3014, issue-3014, body-acquire-rejected-wired) only when
       hp->body_acquire_rejected() — zero extra keys on success / Soft ok.
  AC3  Success / Soft acquire path unchanged: no extra atomic store on
       the hot acq==0 path (store(true) lives in the reject else only).
  AC4  No process-global AgentRegistry; Reclaimed deferred-cleanup
       contract unchanged (complete_agent_join_cleanup / #2661).
  AC5  Additive schema-3014 on query:orch-module-stats. Existing spawn
       quota-reject + try_acquire reject coverage stays green.
  AC6  Extend test_fiber_orch_parallel_quota_batch (#81967); no
       test_issue_3014.cpp; no docs/design/ (#1655).

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
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/serve/test_fiber_orch_parallel_quota_batch.cpp")
    build = _read("build.py")

    # ── AC1: per-handle reject bit stored from the fiber ───────────
    must("Issue #3014", "AC1", spawn)
    must("body_acquire_rejected_slot", "AC1", spawn)
    must("body_acquire_rejected()", "AC1", spawn)
    must("body_acq_rej->store(", "AC1", spawn)
    must("std::make_shared<std::atomic<std::uint8_t>>", "AC1", spawn)

    # ── AC2: join hash keys only on reject ─────────────────────────
    must("body-acquire-rejected", "AC2", agent)
    must("schema-3014", "AC2", agent)
    must("body-acquire-rejected-wired", "AC2", agent)
    gate = agent.find("if (hp->body_acquire_rejected())")
    if gate < 0:
        fails.append("AC2: join hash must gate on hp->body_acquire_rejected()")
    else:
        window = agent[gate : gate + 500]
        if '"body-acquire-rejected"' not in window:
            fails.append("AC2: body-acquire-rejected key must sit inside reject gate")
        if "schema-3014" not in window:
            fails.append("AC2: schema-3014 must sit inside reject gate")

    # ── AC3: success path does not store ───────────────────────────
    acq = spawn.find("if (acq == 0)")
    store = spawn.find("body_acq_rej->store(")
    else_pos = spawn.find("} else {", acq) if acq >= 0 else -1
    if acq < 0 or store < 0 or else_pos < 0 or not (acq < else_pos < store):
        fails.append("AC3: store must be on reject else only (no extra atomic on acq==0)")
    must("Success path does not store", "AC3", spawn)

    # ── AC4: no registry; Reclaimed contract untouched ─────────────
    mark = spawn.find("Issue #3014")
    if mark >= 0 and "AgentRegistry" in spawn[mark : mark + 900]:
        fails.append("AC4: #3014 must not introduce AgentRegistry")
    # Reclaimed cleanup helper must still exist (this issue does not rewrite it).
    must("complete_agent_join_cleanup", "AC4", spawn)
    must("join_reclaimed_deferred_cleanup_total", "AC4", spawn)

    # ── AC5: additive stats ────────────────────────────────────────
    must("schema-3014", "AC5", agent)
    must("issue-3014", "AC5", agent)
    must("agent-body-try-acquire-rejects", "AC5", agent)
    must("schema-1880", "AC5", agent)

    # ── AC6: tests + no invent + no docs/design/ ───────────────────
    must("#3014 AC1", "AC6", test)
    must("body_acquire_rejected()", "AC6", test)
    must("body-acquire-rejected", "AC6", test)
    must("check_body_acquire_rejected_3014", "AC6", build)
    for rel in (
        "tests/orch/test_issue_3014.cpp",
        "tests/compiler/test_issue_3014.cpp",
        "tests/serve/test_issue_3014.cpp",
        "tests/core/test_issue_3014.cpp",
    ):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #81967")
    for rel in (
        "docs/design/3014-body-acquire-rejected.md",
        "docs/design/3014-*.md",
    ):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #3014 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3014 body_acquire_rejected join hash — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
