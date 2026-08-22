#!/usr/bin/env python3
"""Issue #3205: optional :durable mid point-query into WAL.

#3152 forensic-source=3 only pointed at WAL; decision never read
segments. Optional :durable (production + WAL enabled) linearly scans
current + prior rotate segment by join_mid. Soft / no keyword: zero
I/O (existing #3114/#3152 contract). Additive durable-hit + schema-3205.

Contract:
  AC1 optional :durable + find_recent_by_mutation_id (SE WAL first)
  AC2 Soft / no :durable: no scan; production_defaults_active gate
  AC3 planned keys 40; durable-hit / schema-3205 / issue-3205 additive
  AC4 join_mid is the point-query key (no synthetic process-origin mid)
  AC5 extend test_security_audit_unify + 3114 linter; no invent / docs

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

    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    se_wal = _read("src/core/security_event_wal.hh")
    mut_wal = _read("src/core/mutation_audit_wal.hh")
    header = _read("src/compiler/typed_mutation_audit.h")
    unify = _read("tests/compiler/test_security_audit_unify.cpp")
    forensic = _read("tests/compiler/test_evolution_audit_decision_forensic.cpp")
    facade = _read("tests/compiler/test_engine_metrics_facade.cpp")
    l3114 = _read("scripts/coverage/checks/check_evolution_audit_decision_3114.py")
    build = _read("build.py")

    must("kEvolutionAuditDecisionDurableIssue = 3205", "AC1 stamp", header)
    must("Issue #3205", "AC1 handler cite", sec)
    must("find_recent_by_mutation_id", "AC1 SE WAL API", se_wal)
    must("find_recent_by_mutation_id", "AC1 handler SE lookup", sec)
    must("find_recent_by_provenance_mutation_id", "AC1 mutation WAL fallback", mut_wal)
    must("max_segments", "AC1 bounded segments", se_wal)
    must("want_durable", "AC1 :durable parse", sec)
    must("ac3205_1_durable_hit", "AC1 test", unify)

    must("production_defaults_active()", "AC2 production gate", sec)
    must("ac3205_3_soft_quiet", "AC2 Soft test", unify)
    must("ac3205_2_wal_off", "AC2 WAL-off test", unify)
    # Default comment still forbids unsolicited WAL scan.
    must("no should_audit, no WAL scan, no mutate", "AC2 default no scan", sec)
    start = sec.find("query:evolution-audit-decision")
    block = sec[start : start + 18000] if start >= 0 else ""
    if "fopen" in block:
        fails.append("AC2: fopen in evolution-audit-decision handler (I/O belongs in WAL helper)")

    must("kEvolutionAuditDecisionPlannedKeys = 44", "AC3 planned 44 (covers #3242 additive)", sec)
    must('insert_kv("durable-hit", durable_hit)', "AC3 durable-hit", sec)
    must("schema-3205", "AC3 schema-3205", sec)
    must("issue-3205", "AC3 issue-3205", sec)
    must("schema-3205", "AC3 facade", facade)
    must("schema-3205", "AC3 3114 linter", l3114)

    must("join_mid != 0", "AC4 join_mid gate", sec)
    if "process-origin" in block and "synthetic" in block.lower():
        pass
    must("never a synthetic process-origin mid", "AC4 no synthetic mid", sec)

    must("check_evolution_audit_decision_durable_3205", "AC5 build.py", build)
    must("ac6_durable_3205", "AC5 forensic AC", forensic)
    must("3205 AC", "AC5 unify tests", unify)
    if (ROOT / "tests" / "compiler" / "test_issue_3205.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3205.cpp")
    if (ROOT / "tests" / "core" / "test_issue_3205.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3205.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3205-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3205 evolution-audit-decision durable:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3205 evolution-audit-decision durable: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
