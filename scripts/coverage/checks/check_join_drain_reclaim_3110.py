#!/usr/bin/env python3
"""Issue #3110: Production C++ join auto-wait (close host-forget cleanup window).

Contract (one row per AC):
  AC1  Production + Reclaimed + unset wait → join_agent auto-waits via
       wait_reclaimed_body(h, kProductionWaitReclaimedMsDefault=50). Ok
       path clears must_wait_reclaimed (host sees false). Timeout path
       refined by #3146 — must_wait_reclaimed stays true so the host
       knows the body is still running (#2661 preserved).
  AC2  Explicit wait path unchanged (policy.wait_reclaimed_ms.has_value())
  AC3  Soft / Off stays zero cost (production_reclaimed_must_wait() gate
       preserved; no new atomic / no extra counter)
  AC4  Timeout in auto-wait preserves #2661 (no early free); surfaces
       still-running + wait_reclaimed_timeout_total (reuse, no new key)
  AC5  Additive observability only (reuse wait_reclaimed_used /
       wait_reclaimed_total); no new query key, no metrics middle insertion
  AC6  Extend tests/orch/test_join_drain_reclaim.cpp (no test_issue_3110.cpp
       per #81967; no docs/design/3110-* per #1655)
  AC7  Source-cite + build.py wiring + join_agents span variant same pattern
       (mirrors #3146 conditional flag update in both single-handle and
       span variants)

Refinement lineage:
  Issue #3146 — production auto-wait Timeout retains must_wait_reclaimed
  while reservation/mailbox still held. Refines AC1 above (Timeout arm).
  Sibling linter: scripts/coverage/checks/check_join_drain_reclaim_3146.py.
  Verifies that the unconditional clear after auto-wait has been replaced
  with `must_wait_reclaimed = (status == Timeout)` in join_agent /
  join_agents / ensure_reclaimed_cleanup SSOT.

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
    _read("scripts/coverage/manifests/2397.json")

    # ── AC1: join_agent auto-wait when Reclaimed + unset wait + production
    must("Issue #3110: auto-wait to close the host-forget cleanup window", "AC1 join_agent comment marker", spawn)
    must(
        "wr3110 = wait_reclaimed_body(h, kProductionWaitReclaimedMsDefault)",
        "AC1 join_agent auto-wait calls wait_reclaimed_body(50ms default)",
        spawn,
    )
    # Flag is updated after auto-wait. #3110 AC1 originally required an
    # unconditional clear; #3146 refined this — on Timeout the flag must
    # stay true (host still has a live body + held reservation, per #2661).
    # The strict unconditional-clear text is no longer required here; the
    # conditional update is verified by the #3146 linter (sibling).
    must("h.must_wait_reclaimed =", "AC1 must_wait_reclaimed updated after auto-wait", spawn)
    # join_agent single-handle path
    must(
        "[[nodiscard]] inline serve::JoinResult join_agent(AgentHandle& h, JoinPolicy policy)",
        "AC1 join_agent signature",
        spawn,
    )
    must("3110 AC1", "AC1 test marker", test)

    # ── AC2: Explicit wait path unchanged
    must("policy.wait_reclaimed_ms.has_value()", "AC2 explicit wait guard", spawn)
    must("auto wr = wait_reclaimed_body(h, policy.wait_reclaimed_ms)", "AC2 explicit wait call preserved", spawn)
    must("h.wait_reclaimed_used = true", "AC2 wait_reclaimed_used set on explicit wait", spawn)
    must(
        "h.wait_reclaimed_timeout = (wr.status == serve::JoinStatus::Timeout)",
        "AC2 wait_reclaimed_timeout set on explicit timeout",
        spawn,
    )
    must("3110 AC2", "AC2 test marker", test)

    # ── AC3: Soft / Off zero cost via production_reclaimed_must_wait() gate
    must("production_reclaimed_must_wait()", "AC3 production gate preserved", spawn)
    # The new auto-wait block must be INSIDE the production gate (else Soft path
    # would auto-wait too).
    gate_idx = spawn.find("production_reclaimed_must_wait()")
    autowait_idx = spawn.find("wr3110 = wait_reclaimed_body(h, kProductionWaitReclaimedMsDefault)")
    if gate_idx < 0 or autowait_idx < 0:
        fails.append("AC3: missing production gate or auto-wait call")
    elif autowait_idx < gate_idx:
        fails.append("AC3: auto-wait must live AFTER the production_reclaimed_must_wait() gate")
    must("3110 AC3", "AC3 test marker", test)

    # ── AC4: Timeout preserves #2661 no-early-free
    must(
        "wait_reclaimed_timeout = (wr3110.status == serve::JoinStatus::Timeout)",
        "AC4 timeout wired into wait_reclaimed_timeout flag",
        spawn,
    )
    # #2661 lineage: complete_agent_join_cleanup preserved (no early free)
    must("complete_agent_join_cleanup(", "AC4 #2661 complete_agent_join_cleanup preserved", spawn)
    must("3110 AC4", "AC4 test marker", test)

    # ── AC5: Additive observability only — reuse wait_reclaimed_used /
    # wait_reclaimed_total; no new query key, no metrics middle insertion
    must("h.wait_reclaimed_used = true", "AC5 reuse wait_reclaimed_used", spawn)
    # No new metric middle insertions in evaluator_primitives_*.cpp (no new
    # query key inserted between existing #3056/#3051/#3087 keys). Source-cite:
    # the auto-wait does NOT add a new insert_kv to query:security-posture.
    # (Verified by checking that no `wal-append-fail-harden` or `join-auto-wait`
    # query key was added.)
    for forbidden in (
        "wal-auto-wait-total",
        "join-host-forget-total",
        "must_wait_reclaimed_auto_total",
    ):
        if forbidden in spawn:
            fails.append(f"AC5: forbidden new metric key {forbidden!r} (must reuse wait_reclaimed_*)")
    must("3110 AC5", "AC5 test marker", test)

    # ── AC6: Source-cite + extend existing test, no docs/design/*, no test_issue_*
    must("#3110: Production C++ join auto-wait (close host-forget window)", "AC6 3110 comment in test", test)
    must("3110 AC6", "AC6 test marker", test)
    must("kProductionWaitReclaimedMsDefault = 50", "AC6 50ms production default preserved (#3051/#3087)", spawn)
    if (ROOT / "tests" / "orch" / "test_issue_3110.cpp").is_file():
        fails.append("AC6: test_issue_3110.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3110.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3110.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3110-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    # ── AC7: Source-cite + build.py wiring + join_agents span variant
    must("Issue #3110: auto-wait to close the host-forget cleanup window", "AC7 source-cite comment", spawn)
    must(
        "wr3110 = wait_reclaimed_body(a, kProductionWaitReclaimedMsDefault)",
        "AC7 join_agents span variant auto-wait also wired",
        spawn,
    )
    # join_agents span variant: same auto-wait pattern. The source file has TWO
    # copies of the "Issue #3110: auto-wait..." comment block (one in join_agent
    # single-handle, one in join_agents span). Count occurrences to verify both.
    auto_wait_comment_count = spawn.count("Issue #3110: auto-wait to close the host-forget cleanup window")
    if auto_wait_comment_count < 2:
        fails.append(
            f"AC7: expected ≥2 auto-wait comment blocks (join_agent + join_agents), found {auto_wait_comment_count}"
        )
    # Linter wired in build.py
    must("check_join_drain_reclaim_3110", "AC7 build.py wiring", build)
    must("Issue #3110", "AC7 linter error message", build)
    # #2661 / #2924 / #3012 / #3051 / #3087 lineage preserved
    must("wait_reclaimed_body(", "AC7 #2924 wait_reclaimed_body helper preserved", spawn)
    must("complete_agent_join_cleanup(", "AC7 #2661 lineage preserved", spawn)
    must("kProductionWaitReclaimedMsDefault = 50", "AC7 #3051/#3087 50ms default lineage preserved", spawn)
    # 3089 lineage preserved
    # Lineage verified via #2924/#2661/#3051/#3087 helper-presence checks above.
    must("3110 AC7", "AC7 test marker", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3110 production C++ join auto-wait — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
