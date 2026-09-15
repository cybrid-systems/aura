#!/usr/bin/env python3
"""Issue #3806: SE WAL overflow ring wrap/overwrite counter + Agent faces.

Residual of #3109/#3178 — capacity-256 ring advances head with % 256 and
saturates count, but had no wrap/drop counter. Overwritten mids vanish from
wal_overflow_find_by_mid with no Agent-visible breach face.

Contract:
  AC1  security_event_wal_overflow_wrap_total (wal_overflow_ring_wrap_total)
       bumps when push overwrites a live slot (count == capacity before store)
  AC2  Additive Agent keys on security-audit-stats / security-posture /
       evolution-audit-decision / audit-wal-stats:
       wal-overflow-ring-depth + wal-overflow-wrap-total + wal-overflow-full
       (no rename of existing wal-overflow-ring-depth)
  AC3  Soft/Off / WAL-off: zero cost (push never called today)
  AC4  Optional fail-closed after wrap storm is out of scope (observability min)
  AC5  Extends existing WAL/posture tests; linter wired; no invent / no design doc

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SEW = "src/core/security_event_wal.hh"
PRIM = "src/compiler/evaluator_primitives_security.cpp"
TEST = "tests/compiler/test_security_event_wal_replay.cpp"
POSTURE = "tests/compiler/test_security_posture_trail.cpp"
BUILD = "build.py"
LINTER = "check_wal_overflow_wrap_3806"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    sew = _read(SEW)
    prim = _read(PRIM)
    test = _read(TEST)
    posture = _read(POSTURE)
    build = _read(BUILD)

    # AC1 — wrap counter bumped on overwrite.
    must("kWalOverflowWrapIssue = 3806", "AC1 issue stamp", sew)
    must("security_event_wal_overflow_wrap_total", "AC1 counter name", sew)
    must("wal_overflow_ring_wrap_total", "AC1 accessor", sew)
    must("Issue #3806", "AC1 cite in push", sew)
    must(
        "if (expected >= kWalOverflowRingCapacity)\n"
        "        wal_overflow_ring_wrap_total().fetch_add(1, std::memory_order_relaxed);",
        "AC1 bump when count==capacity before store",
        sew,
    )
    must(
        "wal_overflow_ring_wrap_total().store(0, std::memory_order_relaxed);",
        "AC1 clear_for_test resets wrap",
        sew,
    )

    # AC2 — additive Agent faces; depth key not renamed.
    for key in (
        "wal-overflow-ring-depth",
        "wal-overflow-wrap-total",
        "wal-overflow-full",
        "schema-3806",
        "issue-3806",
    ):
        must(f'"{key}"', f"AC2 key {key}", prim)
    # Surfaces cited in AC2.
    must("query:security-audit-stats", "AC2 security-audit-stats surface", prim)
    must("query:security-posture", "AC2 security-posture surface", prim)
    must("query:evolution-audit-decision", "AC2 evolution-audit-decision surface", prim)
    # Depth key meaning preserved (#3109 lineage).
    must("wal-overflow-ring-depth", "AC2 depth key retained", prim)
    must_not("overflow-depth-renamed", "AC2 no rename marker", prim)

    # Count: wrap-total must appear on all four surfaces (posture, audit-wal,
    # audit-stats, evolution). Each insert_kv site once → >= 4.
    wrap_inserts = prim.count('insert_kv("wal-overflow-wrap-total"')
    if wrap_inserts < 4:
        fails.append(f"AC2: expected >=4 wal-overflow-wrap-total insert sites, found {wrap_inserts}")

    # AC3 — Soft/Off zero cost: push still gated by fail-closed.
    must("wal_append_fail_closed_active()", "AC3 fail-closed gate retained", sew)
    # Soft checks in posture trail.
    must("3806 AC3: wrap-total=0 Soft/Off", "AC3 posture Soft check", posture)

    # AC4 — observability minimum only (no wrap-storm deny mandated).
    must_not("wal_overflow_wrap_storm_deny", "AC4 no wrap-storm deny invent", sew)
    must_not("wal_overflow_wrap_storm_deny", "AC4 no wrap-storm deny invent", prim)

    # AC5 — suite + build + no invent.
    must("#3806 AC1/AC2/AC3", "AC5 test block", test)
    must("wal_overflow_ring_wrap_total", "AC5 test uses wrap accessor", test)
    must("wal-overflow-wrap-total", "AC5 test asserts Agent key", test)
    must(LINTER, "AC5 build registration", build)
    for rel in (
        "tests/compiler/test_issue_3806.cpp",
        "tests/core/test_issue_3806.cpp",
        "tests/issues/test_issue_3806.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: forbidden new test file {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3806*"):
            fails.append(f"AC5: docs/design/{p.name} exists")

    # Lineage: #3109/#3178 still wired.
    must("check_wal_append_fail_closed_3109", "AC5 #3109 linter still wired", build)
    must("check_wal_overflow_mid_3178", "AC5 #3178 linter still wired", build)

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3806", file=sys.stderr)
        return 1

    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
