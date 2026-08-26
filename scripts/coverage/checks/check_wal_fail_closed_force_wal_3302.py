#!/usr/bin/env python3
"""Issue #3302: force_wal production path default-arms WAL fail-closed.

#3109 added AURA_WAL_APPEND_FAIL_CLOSED opt-in. Production already
force_wal (Restricted / Strict / multi_tenant) but fail-closed stayed
fail-open unless env was set — durable write + evidence capture unpaired.

Contract (one row per AC):
  AC1  Soft / AURA_SANDBOX=off / production_defaults_active()==0:
       fail-closed inactive; defaulted-by-force-wal=0; no overflow write
  AC2  Restricted or Strict or multi_tenant force_wal (no FAIL_OPEN):
       wal_append_fail_closed_active()==true; inject fail → overflow ring
  AC3  AURA_WAL_APPEND_FAIL_OPEN=1 + force_wal: fail-open + #3056 SLO;
       no overflow push
  AC4  AURA_WAL_APPEND_FAIL_CLOSED=1 still forces on (without FAIL_OPEN)
  AC5  Strict + overflow full: require_effect deny (reason path #3109)
  AC6  Additive query keys; #3109/#3056/#3211 linters still wired;
       no invent test_issue_3302.cpp; no docs/design/3302-*

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
    sd = _read("src/compiler/security_defaults.hh")
    sew = _read("src/core/security_event_wal.hh")
    ev = _read("src/compiler/evaluator_security.cpp")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_security_posture_trail.cpp")
    tdev = _read("src/compiler/typed_mutation_audit.h")
    build = _read("build.py")
    lint3109 = _read("scripts/coverage/checks/check_wal_append_fail_closed_3109.py")
    lint3056 = _read("scripts/coverage/checks/check_wal_append_fail_slo_3056.py")
    lint3211 = _read("scripts/coverage/checks/check_wal_append_fail_schedule_3211.py")

    # ── AC1 Soft zero cost ──────────────────────────────────────────────
    must("aura_production_defaults_active_probe() == 0", "AC1 Soft probe gate", slo)
    must("set_wal_fail_closed_defaulted_by_force_wal(false)", "AC1 Soft clears flag", tdev)
    must("3302 AC1", "AC1 test marker", test)

    # ── AC2 force_wal default pair ──────────────────────────────────────
    must("set_wal_fail_closed_defaulted_by_force_wal(true)", "AC2 arm on force_wal enable", sd)
    must("wal_fail_closed_defaulted_by_force_wal()", "AC2 helper reads flag", slo)
    must("kWalAppendFailClosedForceWalIssue = 3302", "AC2 issue stamp", slo)
    must("3302 AC2", "AC2 test marker", test)

    # ── AC3 explicit opt-out ────────────────────────────────────────────
    must("AURA_WAL_APPEND_FAIL_OPEN", "AC3 opt-out env", slo)
    must('wal_env_flag_truthy("AURA_WAL_APPEND_FAIL_OPEN")', "AC3 OPEN check", slo)
    must("3302 AC3", "AC3 test marker", test)

    # ── AC4 explicit opt-in ─────────────────────────────────────────────
    must('wal_env_flag_truthy("AURA_WAL_APPEND_FAIL_CLOSED")', "AC4 CLOSED check", slo)
    must("3302 AC4", "AC4 test marker", test)

    # ── AC5 Strict overflow full deny (reuse #3109 path) ────────────────
    must(
        "if (req_bits != 0 && ::aura::core::wal_slo::wal_append_fail_closed_active() &&",
        "AC5 deny precondition",
        ev,
    )
    must("wal_overflow_ring_full()", "AC5 overflow full", ev)
    must("is_strict()", "AC5 Strict", ev)
    must("3302 AC5", "AC5 test marker", test)
    must("kWalOverflowRingCapacity = 256", "AC5 ring capacity", sew)

    # ── AC6 query + lineage + no-invent ─────────────────────────────────
    must('insert_kv("wal-fail-closed-defaulted-by-force-wal"', "AC6 additive key", sec)
    must('insert_kv("schema-3302"', "AC6 schema-3302", sec)
    must('insert_kv("issue-3302"', "AC6 issue-3302", sec)
    must('insert_kv("wal-fail-closed-active"', "AC6 3109 key preserved", sec)
    must("check_wal_fail_closed_force_wal_3302", "AC6 build.py wiring", build)
    must("check_wal_append_fail_closed_3109", "AC6 3109 linter still wired", build)
    must("check_wal_append_fail_slo_3056", "AC6 3056 linter still wired", build)
    must("check_wal_append_fail_schedule_3211", "AC6 3211 linter still wired", build)
    must("3109", "AC6 3109 lineage", lint3109)
    must("3056", "AC6 3056 lineage", lint3056)
    must("3211", "AC6 3211 lineage", lint3211)
    must("3302 AC6", "AC6 test marker", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3302.cpp").is_file():
        fails.append("AC6: test_issue_3302.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3302.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3302.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3302-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3302 force_wal default fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
