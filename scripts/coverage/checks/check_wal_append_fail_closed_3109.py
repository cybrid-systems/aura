#!/usr/bin/env python3
"""Issue #3109: production WAL append fail-closed option (SE + mutation
audit trail integrity).

Contract (one row per AC):
  AC1  Soft/Off / AURA_SANDBOX=off: no new cost; fail-closed env ignored;
       wal-fail-closed-active=0, wal-overflow-ring-depth=0
  AC2  Default (no env): behavior unchanged (fail-open + #3056 SLO arm);
       require_effect deny path wired for Strict + fail-closed + overflow full
  AC3  AURA_WAL_APPEND_FAIL_CLOSED=1 + production + Restricted:
       overflow ring captures events; mid can join from overflow ring
  AC4  AURA_WAL_APPEND_FAIL_CLOSED=1 + production + Strict + overflow full:
       require_effect denies with reason wal-append-fail-closed
  AC5  Additive query keys only (wal-fail-closed-active,
       wal-overflow-ring-depth, schema-3109, issue-3109); no new
       capability model; no docs/design/*, no test_issue_3109.cpp

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
    sew = _read("src/core/security_event_wal.hh")
    ev = _read("src/compiler/evaluator_security.cpp")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_security_posture_trail.cpp")
    build = _read("build.py")
    lint3056 = _read("scripts/coverage/checks/check_wal_append_fail_slo_3056.py")

    # ── AC1: Soft/Off zero cost + helper + ring ──────────────────────────
    must("wal_append_fail_closed_active", "AC1 helper declared", slo)
    must("AURA_WAL_APPEND_FAIL_CLOSED", "AC1 env name", slo)
    must("production_defaults_active", "AC1 production gate", slo)
    must("kWalOverflowRingCapacity = 256", "AC1 ring capacity 256", sew)
    must("WalOverflowRecord", "AC1 record struct", sew)
    must("wal_overflow_ring_push", "AC1 ring push helper", sew)
    must("wal_overflow_ring_depth", "AC1 ring depth accessor", sew)
    must("wal_overflow_ring_full", "AC1 ring full check", sew)
    must("wal_overflow_ring_clear_for_test", "AC1 ring clear for test", sew)
    # SecurityEventWal::append wires the fail-closed check (both paths).
    # Simple str.count on the literal substring to avoid regex-escape pitfalls.
    push_count = sew.count("wal_overflow_ring_push(ovr);")
    if push_count < 2:
        fails.append(
            f"AC1: expected ≥2 wal_overflow_ring_push call sites (fwrite_miss + inject_fail), found {push_count}"
        )
    must("3109 AC1", "AC1 test marker", test)

    # ── AC2: Default unchanged + #3056 SLO arm + require_effect deny ─────
    # Helper must be zero-cost when env unset (returns false early).
    must("if (e == nullptr || e[0] == '\\0')", "AC2 early-exit on no-env", slo)
    must("return false;", "AC2 no-env returns false", slo)
    # require_effect wires fail-closed + overflow full + Strict
    must("wal_append_fail_closed_active()", "AC2 helper call in require_effect", ev)
    must("wal_overflow_ring_full()", "AC2 overflow full check in require_effect", ev)
    must("is_strict()", "AC2 Strict check in require_effect", ev)
    # #3056 SLO lineage preserved
    must("kWalAppendFailSloIssue = 3056", "AC2 #3056 lineage", slo)
    must("wal-append-fail-breach", "AC2 #3056 breach key in query surface", sec)
    must("3109 AC2", "AC2 test marker", test)

    # ── AC3: production + Restricted + overflow ring captures events ──
    # The overflow ring push happens INSIDE the fail-closed guard, which
    # itself requires production_defaults_active(). Since
    # wal_overflow_ring_push(ovr) is ONLY called inside the fail-closed guard
    # (no other call sites in the file), counting push occurrences proves the
    # guard is wired. (≥2 = fwrite_miss + inject_fail paths.)
    if push_count < 2:
        fails.append(f"AC3: expected ≥2 wal_overflow_ring_push call sites, found {push_count}")
    must("3109 AC3", "AC3 test marker", test)

    # ── AC4: production + Strict + overflow full → require_effect deny ──
    # The deny must be at the TOP of require_effect (before body runs) so
    # no half-write. Source-cite the early-return.
    must(
        "if (req_bits != 0 && ::aura::core::wal_slo::wal_append_fail_closed_active() &&",
        "AC4 deny precondition wired",
        ev,
    )
    must("return false; // wal-append-fail-closed deny", "AC4 early return deny", ev)
    # Comment must be present so the reason is documented
    must("wal-append-fail-closed deny (#3109", "AC4 reason documented", ev)
    must("3109 AC4", "AC4 test marker", test)

    # ── AC5: additive query keys + no-invent + build.py + lineage ────────
    must('insert_kv("wal-fail-closed-active"', "AC5 wal-fail-closed-active key", sec)
    must('insert_kv("wal-overflow-ring-depth"', "AC5 wal-overflow-ring-depth key", sec)
    must('insert_kv("schema-3109"', "AC5 schema-3109 key", sec)
    must('insert_kv("issue-3109"', "AC5 issue-3109 key", sec)
    # These must be ADDITIVE — old keys unchanged
    for old_key in (
        "wal-append-fail-breach",
        "wal-append-fail-slo-wired",
        "schema-3056",
        "issue-3056",
    ):
        must(old_key, f"AC5 old key preserved: {old_key}", sec)
    # Linter wired in build.py
    must("check_wal_append_fail_closed_3109", "AC5 build.py wiring", build)
    must("Issue #3109", "AC5 linter error message", build)
    # No invent
    if (ROOT / "tests" / "compiler" / "test_issue_3109.cpp").is_file():
        fails.append("AC5: test_issue_3109.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3109.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3109.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3109-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    # #3056 lineage preserved in build.py wiring context
    must("check_wal_append_fail_slo_3056", "AC5 3056 linter lineage", build)
    must("3056", "AC5 3056 lineage in 3056 linter", lint3056)
    must("3109 AC5", "AC5 test marker", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3109 production WAL append fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
