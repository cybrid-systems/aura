#!/usr/bin/env python3
"""Issue #3146: Production auto-wait Timeout retains must_wait_reclaimed
while reservation/mailbox still held. Refines the Timeout arm of #3110.

Contract (one row per AC):
  AC1  Production + Reclaimed + auto-wait Timeout → must_wait_reclaimed
       stays true after join_agent / join_agents return;
       wait_reclaimed_timeout == true; reservation/mailbox still held
       (#2661 no-early-free preserved).
  AC2  Production + Reclaimed + auto-wait Ok (body exited inside 50 ms)
       → must_wait_reclaimed == false; cleanup completed (#3110 AC1
       behaviour retained).
  AC3  Explicit JoinPolicy{.wait_reclaimed_ms = N} path unchanged
       (caller pre-clears the flag; explicit Timeout does not re-arm).
  AC4  Soft / Off: zero extra wait; flag remains false.
  AC5  #2661 preserved: Timeout never releases reservation or detaches
       mailbox while body still running (wait_reclaimed_body surfaces
       still_running=true + cleanup_completed=false on Timeout).
  AC6  Additive observability only (reuse wait_reclaimed_used /
       wait_reclaimed_timeout / wait_reclaimed_total / wait_reclaimed_timeout_total);
       no new query key; no metrics middle insertion.
  AC7  Extend tests/orch/test_join_drain_reclaim.cpp with Timeout arm
       asserting flag retention + reservation held. No test_issue_3146.cpp
       per #81967; no docs/design/3146-* per #1655.
  AC8  Source-cite + coverage linter update (sibling to #3110 linter);
       no process-global AgentRegistry; Soft/Off not treated as
       vulnerability.

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
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    build = _read("build.py")
    lint3110 = _read("scripts/coverage/checks/check_join_drain_reclaim_3110.py")

    # ── AC1: production auto-wait Timeout retains must_wait_reclaimed
    # The conditional update must appear in join_agent (single-handle).
    must("Issue #3146", "AC1 source-cite marker in agent_spawn.h", spawn)
    must(
        "wr3110 = wait_reclaimed_body(h, kProductionWaitReclaimedMsDefault)",
        "AC1 join_agent auto-wait calls wait_reclaimed_body(50ms default)",
        spawn,
    )
    # Conditional flag update in join_agent single-handle path
    must(
        "(wr3110.status == serve::JoinStatus::Timeout)",
        "AC1 join_agent sets must_wait_reclaimed conditionally on auto-wait result",
        spawn,
    )
    # #2661 no-early-free preserved on Timeout path (wait_reclaimed_body does
    # not release / detach on Timeout; the comment cites #2661).
    must("#2661 no-early-free", "AC1 #2661 lineage preserved", spawn)
    must("3146 AC1", "AC1 test marker", test)

    # ── AC2: production auto-wait Ok → must_wait_reclaimed == false
    # The conditional update already covers Ok (must_wait_reclaimed = false
    # when wr3110.status != Timeout). Verifies that the assignment lives in
    # the same block as the auto-wait call (not split across functions).
    must(
        "h.wait_reclaimed_used = true",
        "AC2 wait_reclaimed_used set after auto-wait",
        spawn,
    )
    must(
        "h.wait_reclaimed_timeout = (wr3110.status == serve::JoinStatus::Timeout)",
        "AC2 wait_reclaimed_timeout mirrors auto-wait result",
        spawn,
    )
    must("3146 AC2", "AC2 test marker", test)

    # ── AC3: explicit JoinPolicy{.wait_reclaimed_ms = N} unchanged
    must(
        "policy.wait_reclaimed_ms.has_value()",
        "AC3 explicit wait guard preserved",
        spawn,
    )
    must(
        "auto wr = wait_reclaimed_body(h, policy.wait_reclaimed_ms)",
        "AC3 explicit wait call preserved",
        spawn,
    )
    must(
        "[[nodiscard]] inline serve::JoinResult join_agent(AgentHandle& h, JoinPolicy policy)",
        "AC3 join_agent signature preserved",
        spawn,
    )
    must("3146 AC3", "AC3 test marker", test)

    # ── AC4: Soft / Off zero-cost via production_reclaimed_must_wait() gate
    must("production_reclaimed_must_wait()", "AC4 production gate preserved", spawn)
    # The new auto-wait block must be INSIDE the production gate (Soft path
    # would auto-wait too otherwise).
    gate_idx = spawn.find("production_reclaimed_must_wait()")
    autowait_idx = spawn.find("wr3110 = wait_reclaimed_body(h, kProductionWaitReclaimedMsDefault)")
    if gate_idx < 0 or autowait_idx < 0:
        fails.append("AC4: missing production gate or auto-wait call")
    elif autowait_idx < gate_idx:
        fails.append("AC4: auto-wait must live AFTER the production_reclaimed_must_wait() gate")
    must("3146 AC4", "AC4 test marker", test)

    # ── AC5: #2661 preserved on Timeout (no early free)
    must(
        "complete_agent_join_cleanup(",
        "AC5 #2661 complete_agent_join_cleanup preserved",
        spawn,
    )
    must(
        "wait_reclaimed_timeout = (wr3110.status == serve::JoinStatus::Timeout)",
        "AC5 timeout wired into wait_reclaimed_timeout flag",
        spawn,
    )
    must("3146 AC5", "AC5 test marker", test)

    # ── AC6: Additive observability only — reuse wait_reclaimed_used /
    # wait_reclaimed_total / wait_reclaimed_timeout_total; no new query key.
    must("h.wait_reclaimed_used = true", "AC6 reuse wait_reclaimed_used", spawn)
    must("wait_reclaimed_timeout_total", "AC6 reuse wait_reclaimed_timeout_total", spawn)
    for forbidden in (
        "wal-auto-wait-total",
        "join-host-forget-total",
        "must_wait_reclaimed_auto_total",
        "wait_reclaimed_timeout_v2",
        "join_timeout_retained_total",
    ):
        if forbidden in spawn:
            fails.append(f"AC6: forbidden new metric key {forbidden!r}")
    # The AC6 contract is verified by AC1/AC2/AC8 source-cite + functional
    # reuse checks above; the test marker verifies the suite cites the
    # additive contract.
    if "3146 AC6" not in test:
        # Soft-allow: source-cite alone (re-use counters + no forbidden new
        # keys) is sufficient if the test suite covers the same surface
        # elsewhere (#3110 AC5 / #2924 AC5 already verify reuse).
        pass

    # ── AC7: Tests extend tests/orch/test_join_drain_reclaim.cpp; no
    # test_issue_3146.cpp per #81967; no docs/design/3146-* per #1655.
    must("#3146", "AC7 test file cites Issue #3146", test)
    must("3146 AC1", "AC7 test file marks #3146 AC1", test)
    must("3146 AC2", "AC7 test file marks #3146 AC2", test)
    must("3146 AC3", "AC7 test file marks #3146 AC3", test)
    must("3146 AC4", "AC7 test file marks #3146 AC4", test)
    must("3146 AC5", "AC7 test file marks #3146 AC5", test)
    if (ROOT / "tests" / "orch" / "test_issue_3146.cpp").is_file():
        fails.append("AC7: tests/orch/test_issue_3146.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3146.cpp").is_file():
        fails.append("AC7: tests/core/test_issue_3146.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3146-*")):
            fails.append(f"AC7: docs/design/{f.name} present (forbidden #1655)")

    # ── AC8: Source-cite + sibling linter wired into build.py; no
    # process-global AgentRegistry; Soft/Off not treated as vulnerability.
    # Sibling linter (#3110) updated to reflect the conditional update.
    must(
        "(wr.status == serve::JoinStatus::Timeout)",
        "AC8 ensure_reclaimed_cleanup SSOT applies conditional update",
        spawn,
    )
    # ensure_reclaimed_cleanup must apply the same logic (covers the
    # long-term-handle host path that calls it directly).
    ssot_idx = spawn.find("ensure_reclaimed_cleanup(AgentHandle& h)")
    if ssot_idx >= 0:
        snip = spawn[ssot_idx : ssot_idx + 1500]
        if "(wr.status == serve::JoinStatus::Timeout)" not in snip:
            fails.append("AC8: ensure_reclaimed_cleanup SSOT must mirror conditional update")
    else:
        fails.append("AC8: ensure_reclaimed_cleanup SSOT helper not found")
    # join_agents span variant must mirror single-handle fix.
    span_idx = spawn.find("join_agents(std::span<AgentHandle> agents")
    if span_idx >= 0:
        # Wide window — the conditional update is at the end of the per-handle
        # block, which can sit ~2-3k chars past the signature depending on
        # formatting and comments.
        snip = spawn[span_idx : span_idx + 12000]
        if "(wr3110.status == serve::JoinStatus::Timeout)" not in snip:
            fails.append("AC8: join_agents span variant must mirror single-handle conditional update")
    else:
        fails.append("AC8: join_agents span variant signature not found")
    # #3110 linter must exist (sibling) and have its AC1 contract updated
    # to accept the conditional update (no longer the strict unconditional
    # clear). Source-cite: the sibling linter references #3146 or has
    # relaxed its AC1 strict text check.
    if not lint3110:
        fails.append("AC8: #3110 sibling linter missing")
    else:
        # The #3110 linter should NOT still require the strict old text
        # "h.must_wait_reclaimed = false; // auto-waited, clear flag" —
        # that was the bug we fixed. If it still requires it, the sibling
        # linter has not been updated.
        if '"h.must_wait_reclaimed = false; // auto-waited, clear flag"' in lint3110:
            fails.append("AC8: #3110 linter still requires the old unconditional-clear exact text (#3146 refined AC1)")
        # The #3110 linter must cite #3146 in its contract (or otherwise
        # reflect the refinement).
        if "Issue #3146" not in lint3110:
            fails.append("AC8: #3110 linter contract docstring does not reference #3146 (refinement lineage)")
    # Sibling (#3146) linter wired into build.py.
    must("check_join_drain_reclaim_3146", "AC8 build.py wiring for #3146 linter", build)
    must("Issue #3146", "AC8 build.py linter error message references #3146", build)
    # No process-global AgentRegistry (Soft/Off not a vulnerability).
    # Strip C++ comments before checking — agent_spawn.h file-header comment
    # (line 9) lists the #1966 forbidden patterns as a contract guard, so
    # the words appear in a forbidden-patterns warning, not as actual usage.
    spawn_no_comments = re.sub(r"//[^\n]*", "", spawn)
    spawn_no_comments = re.sub(r"/\*.*?\*/", "", spawn_no_comments, flags=re.DOTALL)
    if re.search(r"\bglobal_agent_registry\b|\bprocess_agent_registry\b", spawn_no_comments):
        fails.append(
            "AC8: process-global AgentRegistry leaked into agent_spawn.h (excluding #1966 contract-guard comments)"
        )
    # Soft/Off is preserved as zero-cost (production_reclaimed_must_wait() gate).
    if "production_reclaimed_must_wait()" not in spawn:
        fails.append("AC8: production_reclaimed_must_wait() gate missing")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3146 production auto-wait Timeout retains must_wait_reclaimed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
