#!/usr/bin/env python3
"""Issue #3838: SE WAL overflow refuse-on-wrap under production fail-closed.

Residual of #3806 — wrap storms were observable (wrap_total + Agent faces)
but push still overwrote live slots → mid join fail-open under production
fail-closed. Refuse the push instead of silent overwrite.

Contract (one row per AC):
  AC1  wal_overflow_ring_push returns false when wrap would overwrite AND
       wal_append_fail_closed_active(); bumps wrap_total + wrap_refuse_total;
       no store (older mids preserved)
  AC2  Agent faces: wal-overflow-wrap-refuse-total + schema/issue-3838 on
       security-audit-stats / security-posture / evolution-audit-decision /
       audit-wal-stats; durable mid miss + wrap_total>0 →
       last-se-reason "overflow_wrap_evicted" (vs empty never-emitted)
  AC3  Soft / WAL-off / fail-open: overwrite path retained (zero-cost Soft);
       no refuse when fail-closed inactive
  AC4  Extends test_security_event_wal_replay.cpp; linter + grandfather +
       build.py wired; no invent test_issue_3838.cpp / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SEW = "src/core/security_event_wal.hh"
PRIM = "src/compiler/evaluator_primitives_security.cpp"
TEST = "tests/compiler/test_security_event_wal_replay.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
LINTER = "check_wal_overflow_wrap_refuse_3838"


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
    build = _read(BUILD)
    gf = _read(GF)

    # AC1 — refuse-on-wrap in push.
    must("kWalOverflowWrapRefuseIssue = 3838", "AC1 issue stamp", sew)
    must("wal_overflow_ring_wrap_refuse_total", "AC1 refuse accessor", sew)
    must("security_event_wal_overflow_wrap_refuse_total", "AC1 counter name", sew)
    must("Issue #3838", "AC1 cite in push", sew)
    must("[[nodiscard]] inline bool wal_overflow_ring_push", "AC1 bool return", sew)
    must(
        "if (expected >= kWalOverflowRingCapacity &&\n"
        "        ::aura::core::wal_slo::wal_append_fail_closed_active()) {\n"
        "        wal_overflow_ring_wrap_refuse_total().fetch_add(1, std::memory_order_relaxed);\n"
        "        return false;\n"
        "    }",
        "AC1 refuse when full+fail-closed",
        sew,
    )
    must(
        "wal_overflow_ring_wrap_refuse_total().store(0, std::memory_order_relaxed);",
        "AC1 clear_for_test resets refuse",
        sew,
    )
    # #3806 wrap bump retained.
    must(
        "if (expected >= kWalOverflowRingCapacity)\n"
        "        wal_overflow_ring_wrap_total().fetch_add(1, std::memory_order_relaxed);",
        "AC1 #3806 wrap bump retained",
        sew,
    )

    # AC2 — Agent faces.
    for key in (
        "wal-overflow-wrap-refuse-total",
        "schema-3838",
        "issue-3838",
        "overflow_wrap_evicted",
    ):
        must(f'"{key}"' if key != "overflow_wrap_evicted" else f'"{key}"', f"AC2 key {key}", prim)
    must('"overflow_wrap_evicted"', "AC2 wrap-evicted reason", prim)
    refuse_inserts = prim.count('insert_kv("wal-overflow-wrap-refuse-total"')
    if refuse_inserts < 4:
        fails.append(f"AC2: expected >=4 wal-overflow-wrap-refuse-total insert sites, found {refuse_inserts}")
    must("wal_overflow_ring_wrap_total()", "AC2 wrap_total gate for evicted face", prim)

    # AC3 — Soft / fail-open overwrite retained (refuse only under fail-closed).
    must("wal_append_fail_closed_active()", "AC3 fail-closed gate", sew)
    must("3838 Soft: overwrite still ok", "AC3 Soft overwrite test", test)
    must("3838 AC4: refuse_total=0 Soft/Off", "AC3 Soft zero refuse", test)

    # AC4 — suite + wiring + no invent.
    must("#3838 AC1/AC2/AC3/AC4", "AC4 test block", test)
    must("wal_overflow_ring_wrap_refuse_total", "AC4 test uses refuse accessor", test)
    must("overflow_wrap_evicted", "AC4 test asserts wrap-evicted face", test)
    must(LINTER, "AC4 build registration", build)
    must("Issue #3838", "AC4 build cite", build)
    must("check_wal_overflow_wrap_refuse_3838.py", "AC4 grandfather basename", gf)
    must(
        "scripts/coverage/checks/check_wal_overflow_wrap_refuse_3838.py",
        "AC4 grandfather path",
        gf,
    )
    # Lineage: #3806 still wired.
    must("check_wal_overflow_wrap_3806", "AC4 #3806 linter still wired", build)
    for rel in (
        "tests/compiler/test_issue_3838.cpp",
        "tests/core/test_issue_3838.cpp",
        "tests/issues/test_issue_3838.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: forbidden new test file {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for pth in design.glob("3838*"):
            fails.append(f"AC4: docs/design/{pth.name} exists")
    must_not("wal_overflow_wrap_storm_deny", "AC4 no #3806-forbidden invent name", sew)

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3838", file=sys.stderr)
        return 1

    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
