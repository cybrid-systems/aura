#!/usr/bin/env python3
"""Issue #2947: mailbox under-boundary wait p99 SLO → security_schedule_gate.

Refine #2903/#2590 — production deny when wait p99 ≥ SLO or starvation
throttle; Soft observe-only; priority never masks commit_not_ready.

Contract (one row per AC):
  AC1  production + high p99/throttle → mailbox_hold_slo deny + counter
  AC2  Soft → would_allow stays true for mailbox reason alone
  AC3  zero samples quiet path (no hist walk in live fill)
  AC4  priority: commit_not_ready before mailbox_hold_slo
  AC5  schema-2947 + deny-mailbox-hold-slo-total; #2590 preserved
  AC6  tests + build.py; no invent/design; #2587 independent

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    gate = _read("src/orch/security_schedule_gate.h")
    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/orch/test_security_schedule_gate.cpp")
    readme = _read("src/orch/README.md")
    build = _read("build.py")
    mbc = _read("src/compiler/evaluator_mutation_boundary.cpp")

    # AC1
    must("Issue #2947", "AC1", gate)
    must("mailbox_hold_slo", "AC1", gate)
    must("mailbox_wait_p99_us", "AC1", gate)
    must("mailbox_starvation_throttled", "AC1", gate)
    must("mailbox_wait_slo_us", "AC1", gate)
    must("mailbox_hold_slo_signal", "AC1", gate)
    must("deny_mailbox_hold_slo_total", "AC1", gate)
    must("kSecurityScheduleMailboxHoldSloIssue", "AC1", gate)
    must("2947 AC1", "AC1", test)

    # AC2
    must("soft_mode", "AC2", gate)
    must("2947 AC2", "AC2", test)

    # AC3
    must("fill_mailbox_hold_slo_live_", "AC3", gate)
    must("two relaxed loads", "AC3", gate)
    must("2947 AC3", "AC3", test)

    # AC4
    must("never masks", "AC4", gate)
    must("commit_not_ready", "AC4", gate)
    must("2947 AC4", "AC4", test)
    # Priority: mailbox after posture in source order of decide()
    pos_posture = gate.find("posture_degraded")
    pos_mailbox = gate.find("mailbox_hold_slo")
    # Use the enum assignment / decide path — last decide arm for mailbox
    decide_start = gate.find("decide_security_schedule")
    decide_body = gate[decide_start : decide_start + 2500] if decide_start >= 0 else ""
    if "posture_degraded" in decide_body and "mailbox_hold_slo" in decide_body:
        if decide_body.find("posture_degraded") > decide_body.find("mailbox_hold_slo"):
            fails.append("AC4: mailbox_hold_slo appears before posture in decide (priority inverted)")
    else:
        # Still require mailbox after posture comment in priority list
        if pos_posture < 0 or pos_mailbox < 0 or pos_mailbox < pos_posture:
            fails.append("AC4: mailbox_hold_slo must rank after posture_degraded in gate header")

    # AC5
    must("schema-2947", "AC5", prim)
    must("issue-2947", "AC5", prim)
    must("deny-mailbox-hold-slo-total", "AC5", prim)
    must("security-schedule-mailbox-hold-slo-wired", "AC5", prim)
    must("schema-2590", "AC5", prim)
    must("mailbox-hold-slo", "AC5", gate)

    # AC6
    must("2947", "AC6", test)
    must("check_mailbox_hold_slo_security_schedule_2947", "AC6", build)
    must("mailbox_hold_slo", "AC6", readme)
    # #2587 independent defense-in-depth
    if "aura_mailbox_starvation_throttled" not in mbc and "aura_orch_mailbox_starvation_throttled" not in mbc:
        fails.append("AC6: #2587 mailbox starvation probe missing from try_acquire path")
    must("make_security_schedule_input_live", "AC6 live", mbc)
    if (ROOT / "tests" / "orch" / "test_issue_2947.cpp").is_file():
        fails.append("AC6: test_issue_2947.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2947-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2947 mailbox hold SLO → security_schedule_gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
