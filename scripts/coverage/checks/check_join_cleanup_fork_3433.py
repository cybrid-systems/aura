#!/usr/bin/env python3
"""Issue #3433: one cleanup policy for "join returned non-Ok and the body is
still running or already marked reclaimed".

join_agent decided cleanup from Fiber::join status, not from post-drain
fiber liveness: a Timeout/Cancelled join whose body is a tight
non-yielding loop (or which became is_reclaimed() after
cancel_and_drain_fiber) still took the Done-path detach + reservation
release while the body may be live — losing mailbox/quota accounting
while the Scheduler still tracks an orphan fiber. join_agents already
derived a per-handle local status (#3050); the single-handle path did
not. This issue re-derives the local status from post-drain fiber
liveness on BOTH paths so the same still-running body gets
Reclaimed-defer (mailbox attached, reservation held) whether join
returned Reclaimed or Timeout/Cancelled.

Contract:
  AC1  production + non-yielding body + join_agent primary Timeout →
       cleanup is Reclaimed-defer (mailbox attached, reservation held)
       until wait_reclaimed_body Done or abandon_reclaimed
  AC2  join Ok + body Done: unchanged Done-path (detach + release)
  AC3  Soft / sandbox=off / explicit wait_reclaimed_ms: no new wait;
       only the local-status re-derive (same cost as join_agents pays)
  AC4  no process-global AgentRegistry; no new query key; reuse
       join_reclaimed_deferred_cleanup_total / host_forget_reclaimed_risk_total
  AC5  extend tests/orch/test_join_drain_reclaim.cpp (no new
       test_issue_NNNN.cpp): Timeout×still-running vs Reclaimed×still-
       running same hold flags; abandon still releases attach-only

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
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")

    # AC1: single-handle join_agent re-derives local status from
    # post-drain fiber liveness (is_reclaimed OR non-Ok + !is_done).
    must("Issue #3433", "AC1 stamp", spawn)
    must("h.fiber->is_reclaimed()", "AC1 liveness reclaim arm", spawn)
    must("!h.fiber->is_done()", "AC1 liveness still-running arm", spawn)
    must("local.status = serve::JoinStatus::Reclaimed", "AC1 derive", spawn)
    must("complete_agent_join_cleanup(h, jr)", "AC1 cleanup routed", spawn)
    must("ac3433_1_timeout_live_defers_like_reclaimed", "AC1 test", test)

    # AC1 must-wait / abandon path reused: production auto-wait +
    # host_forget risk counter (AC4 reuse).
    must("host_forget_reclaimed_risk_total", "AC1/AC4 reuse risk counter", spawn)
    must("wait_reclaimed_body(h, kProductionWaitReclaimedMsDefault)", "AC1 auto-wait", spawn)

    # AC2: Ok + Done unchanged — the derive never fires on Ok.
    must("local.status != serve::JoinStatus::Ok", "AC2 Ok-guard", spawn)
    must("ac3433_2_ok_done_unchanged", "AC2 test", test)

    # AC3: Soft / sandbox=off / explicit wait_reclaimed_ms — no new wait.
    must("production_reclaimed_must_wait()", "AC3 production gate", spawn)
    must("ac3433_3_soft_no_new_wait", "AC3 test", test)

    # AC4: batch join_agents per-handle unified policy + no registry /
    # no new query key. join_agents derivation must mirror single-handle.
    jr_idx = spawn.find("join_agents(std::span<AgentHandle> agents")
    if jr_idx < 0:
        fails.append("AC4: join_agents span variant present")
    else:
        snip = spawn[jr_idx:]
        must("a.fiber->is_reclaimed()", "AC4 per-handle reclaim arm", snip)
        must("!a.fiber->is_done()", "AC4 per-handle still-running arm", snip)
        must("local.status = serve::JoinStatus::Reclaimed", "AC4 derive", snip)
    if "class AgentRegistry" in spawn or "struct AgentRegistry" in spawn:
        fails.append("AC4: process-global AgentRegistry present (forbidden)")
    if "query:join-cleanup" in spawn or "query:reclaim-live" in spawn:
        fails.append("AC4: new query key invented (forbidden)")
    must("ac3433_4_batch_unified_and_held_flags", "AC4 test", test)

    # AC5: extend test_join_drain_reclaim.cpp; abandon attach-only;
    # linter wired AFTER #3334; no test_issue / no docs/design.
    must("ac3433_5_abandon_attach_only", "AC5 abandon test", test)
    must("ac3433_6_source_and_linter", "AC5 source-cite test", test)
    must("check_join_cleanup_fork_3433", "AC5 build.py", build)
    prev = build.find("check_reclaimed_abandon_3334")
    ours = build.find("check_join_cleanup_fork_3433")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3334")
    if (ROOT / "tests" / "orch" / "test_issue_3433.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_3433.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3433.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3433.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3433-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3433 join cleanup fork — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
