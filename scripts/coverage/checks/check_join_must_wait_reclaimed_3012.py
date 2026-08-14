#!/usr/bin/env python3
"""Issue #3012: production Reclaimed + unset wait → must-wait-reclaimed.

Contract (one row per AC):
  AC1  Production defaults (Restricted/Strict + production_defaults_active,
       AURA_SANDBOX!=off) + wait_reclaimed_ms unset after Reclaimed:
       AgentHandle.must_wait_reclaimed = true. No auto-wait (wait_reclaimed_
       total not bumped). #2661: join path does not release reservation.
  AC2  Soft / AURA_SANDBOX=off / explicit wait_reclaimed_ms stay zero-cost
       (no extra wait, no extra wait_reclaimed_* bump from this issue).
  AC3  ~AgentHandle / finish_reclaimed_cleanup_on_dtor: if body is Done,
       run Done-path complete_agent_join_cleanup; always release
       reservation (no permanent leak). Does not free body-stack while
       still running (#2661).
  AC4  Aura Reclaimed hash: must-wait-reclaimed + schema-3012 +
       must-wait-reclaimed-wired. Documented mild deadline
       kProductionWaitReclaimedMsDefault (not auto-injected).
  AC5  Metrics reuse wait_reclaimed_* (#2924/#2970); additive schema-3012
       on query:orch-module-stats. No new process-global registry.
  AC6  Extend test_join_drain_reclaim (#81967); no test_issue_3012.cpp;
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
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")

    # ── AC1: production unset wait → must_wait_reclaimed, no inject ─
    must("Issue #3012", "AC1", spawn)
    must("must_wait_reclaimed", "AC1", spawn)
    must("production_reclaimed_must_wait", "AC1", spawn)
    must("wait_reclaimed_ms.has_value()", "AC1", spawn)
    if "wait_reclaimed_body(h, policy.wait_reclaimed_ms)" not in spawn:
        fails.append("AC1: must not replace #2970 explicit wait path")

    # ── AC2: Soft / sandbox=off short-circuit ──────────────────────
    must("AURA_SANDBOX", "AC2", spawn)
    must("sandbox=off", "AC2", spawn)

    # ── AC3: dtor finish ───────────────────────────────────────────
    must("finish_reclaimed_cleanup_on_dtor", "AC3", spawn)
    must("complete_agent_join_cleanup(*this, done_jr)", "AC3", spawn)
    dtor = spawn.find("~AgentHandle()")
    finish_in_dtor = spawn.find("finish_reclaimed_cleanup_on_dtor()", dtor) if dtor >= 0 else -1
    if dtor < 0 or finish_in_dtor < 0:
        fails.append("AC3: ~AgentHandle must call finish_reclaimed_cleanup_on_dtor")

    # ── AC4: Aura hash + documented deadline ───────────────────────
    must("must-wait-reclaimed", "AC4", agent)
    must("schema-3012", "AC4", agent)
    must("must-wait-reclaimed-wired", "AC4", agent)
    must("kProductionWaitReclaimedMsDefault", "AC4", spawn)

    # ── AC5: reuse wait_reclaimed_* + additive schema ──────────────
    must("wait_reclaimed_total", "AC5", spawn)
    must("schema-3012", "AC5", agent)
    must("production-wait-reclaimed-ms-default", "AC5", agent)
    if "AgentRegistry" in spawn[spawn.find("Issue #3012") : spawn.find("Issue #3012") + 800]:
        fails.append("AC5: #3012 must not introduce AgentRegistry")

    # ── AC6: tests + no invent + no docs/design/ ───────────────────
    must("#3012 AC1", "AC6", test)
    must("check_join_must_wait_reclaimed_3012", "AC6", build)
    must("#3012", "AC6", spawn)
    for rel in ("tests/orch/test_issue_3012.cpp", "tests/compiler/test_issue_3012.cpp"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #81967")
    for rel in (
        "docs/design/3012-must-wait-reclaimed.md",
        "docs/design/3012-*.md",
    ):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #3012 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3012 production must-wait-reclaimed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
