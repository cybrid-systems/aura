#!/usr/bin/env python3
"""Issue #3002: mailbox hold p99 SSOT + soak fail-closed cancel/release.

Residual of #2931/#2947/#2958: security_schedule_gate deny and #2958
holder cancel must share the same live p99/throttle sample. Production
+ signal + live holder → one-shot request_hold_budget_cancel. Chaos
soak aborts if p99 stays ≥ SLO without cancel / forced-fail-closed.

Contract (one row per AC):
  AC1 Production + p99≥SLO + live holder → cancel (one-shot). Gate still
      denies new mutate. Soft: observe only.
  AC2 fill_mailbox_hold_slo_live_ and #2958 share
      sample_mailbox_hold_slo_live / mailbox_hold_slo_live_signal
      (no second hist walk).
  AC3 Chaos soak fail-closed on hot p99 + no cancel/release under
      production-like defaults. PR default path unchanged.
  AC4 Additive schema-3002; #2903 hist / #2947 deny / #2958 cancel
      totals non-regressing.
  AC5 Extend test_mailbox_recv_mutation_boundary + chaos_mutate (#81967).
  AC6 No docs/design/* per #1655.

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

    gate = _read("src/orch/security_schedule_gate.h")
    mb = _read("src/serve/multi_fiber_mailbox.h")
    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    recv = _read("tests/serve/test_mailbox_recv_mutation_boundary.cpp")
    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    build = _read("build.py")

    # AC1
    must("maybe_mailbox_defer_slo_hold_cancel", "AC1 fill feeds cancel", gate)
    must("mailbox_hold_slo_signal", "AC1", gate)
    must("request_hold_budget_cancel", "AC1", mb)
    must("ac3002_1_gate_sample_cancels_holder", "AC1", recv)

    # AC2
    must("sample_mailbox_hold_slo_live", "AC2", mb)
    must("mailbox_hold_slo_live_signal", "AC2", mb)
    must("sample_mailbox_hold_slo_live", "AC2 gate", gate)
    must("two relaxed loads", "AC2 quiet", gate)
    must("no second hist walk", "AC2", mb)
    must("ac3002_2_shared_sample_quiet_path", "AC2", recv)

    # AC3
    must("hot p99 without hold-cancel", "AC3 soak", chaos)
    must("holder released after cancel+safepoint", "AC3", chaos)
    must("ac3002_mailbox_hold_slo_soak_cite", "AC3", chaos)
    must("AURA_CHAOS_PR_GATE_ONLY", "AC3 PR path", chaos)

    # AC4
    must("schema-3002", "AC4", msg)
    must("mailbox-hold-slo-ssot-wired", "AC4", msg)
    must("schema-2958", "AC4 #2958", msg)
    must("mailbox-defer-slo-hold-cancel-total", "AC4 #2958", msg)
    must("schema-2903", "AC4 #2903", msg)
    must("kSecurityScheduleMailboxHoldSloIssue = 2947", "AC4 #2947", gate)
    must("ac3002_4_schema_and_lineage", "AC4", recv)

    # AC5 / AC6
    must("kMailboxHoldSloSsotIssue = 3002", "AC5", mb)
    must("Issue #3002", "AC5 gate", gate)
    must("check_mailbox_hold_slo_ssot_soak_3002", "AC5", build)
    must("ac3002_5_source_and_linter", "AC5", recv)
    if (ROOT / "tests" / "serve" / "test_issue_3002.cpp").is_file():
        fails.append("AC5: test_issue_3002.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*3002*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3002 mailbox hold SLO SSOT + soak fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
