#!/usr/bin/env python3
"""Issue #3056: production WAL append_fail arms security-posture degraded.

Contract (one row per AC):
  AC1  WAL disabled / Soft → append still short-circuits on is_enabled();
       no degraded arm from this residual
  AC2  production + WAL enabled + append fails past SLO →
       query:security-posture exposes wal-append-fail-breach; counters bump
  AC3  mutation commit stays fail-open ((void)append)
  AC4  additive schema only (wal-append-fail-breach); no rename of append-fail
  AC5  mutation_audit_wal and security_event_wal share decide_wal_append_fail_slo
  AC6  Source-cite + coverage linter; no docs/design/ (#1655);
       no test_issue_3056.cpp (#81967); no second metrics bus

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

    slo = _read("src/core/wal_append_fail_slo.h")
    mut = _read("src/core/mutation_audit_wal.hh")
    se = _read("src/core/security_event_wal.hh")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    evsec = _read("src/compiler/evaluator_security.cpp")
    test = _read("tests/compiler/test_security_event_wal_replay.cpp")
    trail = _read("tests/compiler/test_security_posture_trail.cpp")
    build = _read("build.py")

    must("kWalAppendFailSloIssue = 3056", "AC1 stamp", slo)
    must("note_wal_append_fail", "AC1 note", slo)
    must("consume_wal_inject_append_fail", "AC1 inject", slo)
    must("if (!enabled || !fp)", "AC1 mut short-circuit", mut)
    must("if (!enabled || !fp)", "AC1 se short-circuit", se)
    must("3056 AC1", "AC1 test", test)

    must("wal-append-fail-breach", "AC2 posture key", sec)
    must("schema-3056", "AC2 schema", sec)
    must("would_arm_degraded", "AC2 decide", slo)
    must("3056 AC2", "AC2 test", test)

    must("(void)g_mutation_audit_wal().append", "AC3 fail-open", evsec)
    must("Issue #3056", "AC3 cite", evsec)
    must("3056 AC3", "AC3 test", test)

    must("append-fail", "AC4 existing key", sec)
    must("wal-append-fail-total", "AC4 existing SE key", obs)
    must("3056 AC4", "AC4 test", test)

    must("decide_wal_append_fail_slo", "AC5 decide", slo)
    must("note_wal_append_fail", "AC5 mut feed", mut)
    must("note_wal_append_fail", "AC5 se feed", se)
    must("3056 AC5", "AC5 test", test)

    must("check_wal_append_fail_slo_3056", "AC6 build", build)
    must("3056 AC6", "AC6 test", test)
    must("schema-3056", "AC6 trail", trail)
    if _read("docs/design/3056-wal-append-fail-slo.md"):
        fails.append("AC6: docs/design/3056-* present")
    if _read("tests/compiler/test_issue_3056.cpp"):
        fails.append("AC6: test_issue_3056.cpp present")
    if "class WalAppendFailBus" in slo or "g_wal_append_fail_ring_3056" in slo:
        fails.append("AC6: second metrics bus introduced")

    if fails:
        print(f"Issue #3056 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3056 WAL append_fail SLO — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
