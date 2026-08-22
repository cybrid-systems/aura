#!/usr/bin/env python3
"""Issue #3211: production WAL append-fail SLO → security-schedule-gate deny.

#3056 armed posture only; admit stayed fail-open. This residual folds
would_arm_degraded into decide_security_schedule.

Contract (one row per AC):
  AC1  SecurityScheduleInput.wal_append_fail_would_arm + live helper
       from existing SLO counters (pure decide).
  AC2  production && would_arm → deny force_reason wal-append-fail-breach;
       Soft never hard-deny.
  AC3  make_security_schedule_input_live fills the signal; try_acquire
       / try_acquire_for_region already consume via admit_security_schedule.
  AC4  query:security-schedule-gate additive keys wal-append-fail-breach,
       would-deny-admit, schema-3211; schema-2590 preserved.
  AC5  WAL-off / no-fail: live helper two relaxed loads then false.
  AC6  tests extend test_security_schedule_gate; linter wired; no invent;
       no extra metrics bus.

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
    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    mbc = _read("src/compiler/evaluator_mutation_boundary.cpp")
    slo = _read("src/core/wal_append_fail_slo.h")
    test = _read("tests/orch/test_security_schedule_gate.cpp")
    readme = _read("src/orch/README.md")
    build = _read("build.py")

    # AC1
    must("kSecurityScheduleWalAppendFailIssue = 3211", "AC1 stamp", gate)
    must("wal_append_fail_would_arm", "AC1 input", gate)
    must("wal_append_fail_would_arm_live", "AC1 live helper", gate)
    must("wal_append_fail_breach", "AC1 enum", gate)
    must("decide_wal_append_fail_slo", "AC1 reuse SLO decide", gate)
    must("3211 AC1", "AC1 test", test)

    # AC2
    must("wal-append-fail-breach", "AC2 force_reason string", gate)
    must("deny_wal_append_fail_breach_total", "AC2 counter", gate)
    must("3211 AC2", "AC2 test", test)
    must("Soft never denies", "AC2 Soft test", test)

    # AC3
    must("wal_append_fail_would_arm = wal_append_fail_would_arm_live", "AC3 live fill", gate)
    must("make_security_schedule_input_live", "AC3 try_acquire", mbc)
    must("admit_security_schedule", "AC3 admit", mbc)
    must("wal_append_fail_would_arm_live", "AC3 boundary cite", mbc)
    must("3211 AC3", "AC3 test", test)

    # AC4
    must("wal-append-fail-breach", "AC4 query key", prim)
    must("would-deny-admit", "AC4 query would-deny-admit", prim)
    must("schema-3211", "AC4 schema", prim)
    must("schema-2590", "AC4 2590 preserved", prim)
    must("3211 AC4", "AC4 test", test)

    # AC5
    must("consecutive.load(std::memory_order_relaxed) == 0", "AC5 quiet consecutive", gate)
    must("combined_fail_total.load(std::memory_order_relaxed) == 0", "AC5 quiet combined", gate)
    must("3211 AC5", "AC5 test", test)

    # AC6
    must("check_wal_append_fail_schedule_3211", "AC6 build", build)
    must("wal-append-fail-breach", "AC6 README", readme)
    must("Issue #3211", "AC6 SLO cite", slo)
    must("3211 AC6", "AC6 test", test)
    if _read("docs/design/3211-wal-append-fail-schedule.md"):
        fails.append("AC6: docs/design/3211-* present (forbidden per #1655)")
    if (ROOT / "tests" / "orch" / "test_issue_3211.cpp").is_file():
        fails.append("AC6: tests/orch/test_issue_3211.cpp present (forbidden per #81967)")
    if "class WalAppendFailScheduleBus" in gate or "g_wal_append_fail_ring_3211" in gate:
        fails.append("AC6: extra metrics bus introduced")

    # Priority: wal_append after posture, mailbox still last
    decide_start = gate.find("decide_security_schedule")
    decide_body = gate[decide_start : decide_start + 3500] if decide_start >= 0 else ""
    pos_posture = decide_body.find("posture_wal_off_restricted")
    pos_wal = decide_body.find("wal_append_fail_would_arm")
    pos_mailbox = decide_body.find("mailbox_hold_slo_signal")
    if not (pos_posture != -1 and pos_wal != -1 and pos_mailbox != -1 and pos_posture < pos_wal < pos_mailbox):
        fails.append("AC2: decide priority must be posture < wal_append_fail < mailbox")

    if fails:
        print(f"Issue #3211 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3211 WAL append-fail schedule deny — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
