#!/usr/bin/env python3
"""Issue #3780: emit_mutation_audit WAL miss fail-closed before Occurrence
persist (residual of #3734 post-commit overflow-only).

Contract (one row per AC):
  AC1  Under wal_append_fail_closed_active() + mutation WAL enabled,
       MutationBoundaryGuard dtor appends/denies BEFORE
       aura_outermost_success_persist_occurrence; emit returns false on
       miss and overflow still stamps mutation_wal_append_miss
  AC2  check_and_record_effect / require_effect #3639 same-mutate deny
       before body unchanged (wal_append_missed return false)
  AC3  Soft / WAL-off: fail-open preserved — exactly one bare
       (void)g_mutation_audit_wal().append(rec); remains (late emit path);
       Guard pre-persist gate gated on fail_closed + is_enabled
  AC4  No query key rename; overflow reason mutation_wal_append_miss
       stable; no test_issue_3780.cpp; no docs/design/3780-*; ACs live in
       tests/core/test_audit_replay_join.cpp

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    sec = _read("src/compiler/evaluator_security.cpp")
    bnd = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/core/test_audit_replay_join.cpp")
    build = _read("build.py")

    # ── AC1: pre-persist gate + emit returns false ─────────────────────
    must("[[nodiscard]] bool emit_mutation_audit", "AC1 bool signature", ixx)
    must("bool Evaluator::emit_mutation_audit", "AC1 emit def", sec)
    must("Issue #3780", "AC1 #3780 cite in emit", sec)
    must("return false;", "AC1 emit returns false on miss", sec)
    must('ovr.reason = "mutation_wal_append_miss"', "AC1 overflow reason", sec)
    gate = bnd.find("Issue #3780")
    if gate < 0:
        fails.append("AC1: Guard dtor missing Issue #3780 cite")
    else:
        persist = bnd.find("aura_outermost_success_persist_occurrence", gate)
        if persist < 0:
            fails.append("AC1: #3780 gate must precede aura_outermost_success_persist_occurrence")
    must("wal_append_fail_closed_active()", "AC1 fail-closed gate", bnd)
    must("g_mutation_audit_wal().is_enabled()", "AC1 WAL enabled gate", bnd)
    must("emit_mutation_audit(nodes_changed_wal", "AC1 pre-persist emit call", bnd)

    # ── AC2: #3639 unchanged ───────────────────────────────────────────
    must(
        "wal_append_missed && ::aura::core::wal_slo::wal_append_fail_closed_active()",
        "AC2 #3639 same-mutate deny",
        sec,
    )
    must("wal_append_missed = true", "AC2 miss flag", sec)
    must('ovr.reason = std::string("mutation_wal_append_miss");', "AC2 effect overflow reason", sec)

    # ── AC3: Soft fail-open ────────────────────────────────────────────
    bare = sec.count("(void)g_mutation_audit_wal().append(rec);")
    if bare != 1:
        fails.append(f"AC3: expected exactly 1 bare (void) mutation append (Soft observe), found {bare}")
    must("Soft / WAL-off keep this post-success emit", "AC3 late emit Soft comment", bnd)
    must("g_tls_mutation_audit_wal_precommitted", "AC3 TLS precommit skip", bnd)
    must("Issue #3056", "AC3 #3056 cite", sec)

    # ── AC4: stable keys / no invent ───────────────────────────────────
    must("mutation_wal_append_miss", "AC4 reason stable", sec)
    must("3780 AC1", "AC4 test marker AC1", test)
    must("3780 AC2", "AC4 test marker AC2", test)
    must("3780 AC4", "AC4 test marker AC4", test)
    must("ac17_pre_persist_wal_miss_fail_closed_3780", "AC4 test fn", test)
    must("check_mutation_wal_miss_fail_closed_3780", "AC4 build.py wiring", build)
    must_not("schema-3780", "AC4 no schema-3780", test)
    must_not("schema-3780", "AC4 no schema-3780 in sec", sec)
    if (ROOT / "tests" / "core" / "test_issue_3780.cpp").is_file():
        fails.append("AC4: test_issue_3780.cpp present (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3780.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3780.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("3780*"):
            fails.append(f"AC4: forbidden design doc {f.name}")

    if fails:
        print("FAIL check_mutation_wal_miss_fail_closed_3780:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK check_mutation_wal_miss_fail_closed_3780 (AC1-AC4)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
