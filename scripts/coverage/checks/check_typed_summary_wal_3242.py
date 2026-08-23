#!/usr/bin/env python3
"""Issue #3242: durable typed summary sidecar after typed-trail wrap.

Contract:
  AC1  TypedSummaryWalRecord sidecar (own magic/version); persist on
       capture_audit_event_forced when production + mutation WAL
  AC2  query:security-audit / query:evolution-audit-decision additive
       typed-summary-from-wal keys on miss; old keys unchanged
  AC3  AuditWalRecord size/magic unchanged; new counter at struct end
  AC4  Soft / WAL-off / Sampled-skip: no sidecar write
  AC5  tests extend test_security_audit_unify; no invent / docs/design

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

    wal = _read("src/core/mutation_audit_wal.hh")
    header = _read("src/compiler/typed_mutation_audit.h")
    hooks = _read("src/compiler/typed_mutation_audit_hooks.cpp")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    unify = _read("tests/compiler/test_security_audit_unify.cpp")
    facade = _read("tests/compiler/test_engine_metrics_facade.cpp")
    build = _read("build.py")

    must("kTypedSummaryWalIssue = 3242", "AC1 stamp header", header)
    must("kTypedSummaryWalMagic", "AC1 sidecar magic", wal)
    must("'T', 'Y', 'S', '1'", "AC1 magic bytes", wal)
    must("struct TypedSummaryWalRecord", "AC1 record", wal)
    must("append_typed_summary", "AC1 append", wal)
    must("find_recent_typed_summary_by_mid", "AC1 lookup", wal)
    must("typed-summary-", "AC1 sidecar filename", wal)
    must("maybe_persist_typed_summary", "AC1 persist hook decl", header)
    must("append_typed_summary", "AC1 persist from trail", hooks)
    must("production_defaults_active()", "AC1 production gate", hooks)

    must("typed-summary-from-wal", "AC2 security-audit key", sec)
    must("typed-outcome-wal", "AC2 security-audit outcome", sec)
    must('insert_kv("typed-summary-from-wal", typed_summary_from_wal)', "AC2 evolution key", sec)
    must('insert_kv("typed-kind", typed_kind)', "AC2 typed-kind", sec)
    must("typed_kind =", "AC2 do not rewrite old typed_kind/typed_outcome names", sec)
    must("kEvolutionAuditDecisionPlannedKeys = 56", "AC2 planned 48", sec)

    must("kAuditWalMagic[8]", "AC3 mutation magic unchanged", wal)
    must("kAuditWalVersion = 1", "AC3 version 1", wal)
    must("typed_summary_wal_persisted_total", "AC3 WAL metric end", wal)
    must("typed_summary_wal_persisted_total", "AC3 typed counters end", header)
    must("schema-3242", "AC3 schema-3242", sec)
    if "kAuditWalVersion = 2" in wal:
        fails.append("AC3: mutated mutation WAL version (must stay 1)")

    must("is_enabled()", "AC4 WAL-off short-circuit", hooks)
    must("ac3242_2_soft", "AC4 Soft test", unify)
    must("ac3242_3_mid0", "AC4 mid=0 test", unify)

    must("ac3242_1_wal_hit", "AC5 AC1 test", unify)
    must("schema-3242", "AC5 facade", facade)
    must("check_typed_summary_wal_3242", "AC5 build.py", build)
    if (ROOT / "tests" / "core" / "test_issue_3242.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3242.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3242.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3242.cpp present (forbidden #81967)")
    if _read("docs/design/3242-typed-summary-wal.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3242 typed_summary_wal:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3242 typed_summary_wal: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
