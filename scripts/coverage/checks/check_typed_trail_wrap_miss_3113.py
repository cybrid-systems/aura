#!/usr/bin/env python3
"""Issue #3113: typed trail 256 wrap vs SE ring 1024 + WAL.

Contract (one row per AC):
  AC1  query:security-audit additive typed-trail-miss / typed-trail-size=256
       / se-ring-size=1024. No rename/delete of existing keys (schema=2054,
       typed_kind, typed_outcome, typed-trail-size live occupancy on stats).
  AC2  wal-replay-hint=1 when typed miss + mutation or SE WAL is_enabled().
       Soft / WAL off: no disk scan (is_enabled only).
  AC3  typed_trail_wrap_total bumps when seq crosses 256. stats/posture
       expose typed-trail-wrap-risk when trail_seq>256 and recent SE.
  AC4  Comment/schema: in-memory join is last kTypedMutationAuditTrailSize;
       full replay is SE 1024 + WAL. 256+WAL is the production contract
       (AURA_TYPED_TRAIL_SIZE deferred, does not block AC1–3).
  AC5  Optional env resize deferred; document keep 256+WAL.
  AC6  Soft Sampled/Off same as today; miss mark OK, no WAL scan.

Extend tests/compiler/test_security_audit_unify.cpp. No test_issue_3113.cpp.
No docs/design/3113-* per #1655.

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

    header = _read("src/compiler/typed_mutation_audit.h")
    query = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_security_audit_unify.cpp")
    build = _read("build.py")

    # ── AC1: additive miss + window + se-ring on query:security-audit ──
    must("typed-trail-miss", "AC1 row miss key", query)
    must("typed-trail-size={}", "AC1 row window key", query)
    must("se-ring-size={}", "AC1 row se-ring-size key", query)
    must("schema={}", "AC1 existing schema=2054 retained", query)
    must("typed_kind", "AC1 existing typed_kind retained", query)
    must("typed_outcome", "AC1 existing typed_outcome retained", query)
    must("3113 AC1", "AC1 test marker", test)

    # ── AC2: WAL hint is is_enabled() only ──
    must("wal-replay-hint", "AC2 wal-replay-hint key", query)
    must("g_mutation_audit_wal().is_enabled()", "AC2 mutation WAL is_enabled", query)
    must("g_security_event_wal().is_enabled()", "AC2 SE WAL is_enabled", query)
    must("no disk scan", "AC2 no disk scan comment", query)
    must("3113 AC2", "AC2 test marker", test)

    # ── AC3: wrap counter + wrap-risk ──
    must("typed_trail_wrap_total", "AC3 wrap counter in header", header)
    must("typed-trail-wrap-risk", "AC3 wrap-risk on stats/posture", query)
    must("typed-trail-wrap-total", "AC3 wrap-total on stats/posture", query)
    must("typed-trail-window", "AC3 window on stats", query)
    must("schema-3113", "AC3 schema-3113", query)
    must("3113 AC3", "AC3 test marker", test)

    # ── AC4: join contract comments ──
    must("kTypedTrailWrapMissIssue", "AC4 issue stamp", header)
    must("in-memory typed join window", "AC4 window contract comment", header)
    must("kTypedMutationAuditTrailSize", "AC4 window constant cited", query)
    must("3113 AC4", "AC4 test marker", test)

    # ── AC5: keep 256+WAL, no env resize ──
    must("AURA_TYPED_TRAIL_SIZE deferred", "AC5 env resize deferred", header)
    if "AURA_TYPED_TRAIL_SIZE" in query:
        fails.append("AC5: query path must not read AURA_TYPED_TRAIL_SIZE")
    must("3113 AC5", "AC5 test marker", test)

    # ── AC6: Soft / WAL-off no extra I/O ──
    must("3113 AC6", "AC6 test marker", test)
    must("WAL-off stay zero extra I/O", "AC6 Soft/WAL-off comment", query)

    # ── extend existing suite, no invent, no docs/design ──
    must("check_typed_trail_wrap_miss_3113", "linter wired in build.py", build)
    must("Issue #3113", "linter error message", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3113.cpp").is_file():
        fails.append("AC: tests/compiler/test_issue_3113.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "serve" / "test_issue_3113.cpp").is_file():
        fails.append("AC: tests/serve/test_issue_3113.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3113-*")):
            fails.append(f"AC: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3113 typed trail wrap miss — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
