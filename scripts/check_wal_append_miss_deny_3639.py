#!/usr/bin/env python3
"""Issue #3639: same-mutate WAL append-miss fail-closed (#3493/#3211
residual).

Before this fix, require_effect denied only when the #3109 overflow ring
was FULL (#3493); a mutation WAL fwrite miss with ring-not-full stayed
fail-open ((void)append, #3056) — the side effect landed while durable
evidence could be lost, and the #3211 SLO only denied the NEXT outermost
mutate.

Contract (one row per AC):
  AC1  check_and_record_effect consumes the mutation WAL append result;
       a miss under wal_append_fail_closed_active() denies THIS mutate
       (return false before body / Guard write) and the overflow ring
       still captures the record (#3109 caller pattern, #3178 stamps)
  AC2  #3493 overflow-full entry deny in require_effect unchanged (no
       is_strict() conjunct)
  AC3  Soft / production_defaults_active()==0 keeps #3056 fail-open:
       the deny is gated on wal_append_fail_closed_active(); exactly one
       bare (void)append remains (emit_mutation_audit observe path)
  AC4  AURA_WAL_APPEND_FAIL_OPEN opt-out env intact (wal_append_fail_slo.h)
  AC5  #3211 next-outermost schedule-gate intact (header contract +
       check_wal_append_fail_schedule_3211 wired + regression binary)
  AC6  ACs live in tests/compiler/test_security_posture_trail.cpp
       (#3639 markers); 3109/3302 linters carry 3639 rows; no
       tests/**/test_issue_3639.cpp; no docs/design/3639-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ev = _read("src/compiler/evaluator_security.cpp")
    slo = _read("src/core/wal_append_fail_slo.h")
    test = _read("tests/compiler/test_security_posture_trail.cpp")
    build = _read("build.py")
    lint3109 = _read("scripts/coverage/checks/check_wal_append_fail_closed_3109.py")
    lint3302 = _read("scripts/coverage/checks/check_wal_fail_closed_force_wal_3302.py")

    # ── AC1: gate consumes the append result + same-mutate deny ─────────
    must("if (!g_mutation_audit_wal().append(rec))", "AC1 gate consumes append", ev)
    must("wal_append_missed", "AC1 miss flag", ev)
    deny_idx = ev.find("if (ok && wal_append_missed")
    if deny_idx < 0:
        fails.append("AC1: same-mutate deny if missing")
    else:
        deny_window = ev[deny_idx : deny_idx + 240]
        if "wal_append_fail_closed_active()" not in deny_window:
            fails.append("AC1: deny not gated on fail-closed")
        if "return false" not in deny_window:
            fails.append("AC1: same-mutate deny does not return false")
    assign_idx = ev.find("wal_append_missed = true")
    push_idx = ev.find('ovr.reason = std::string("mutation_wal_append_miss");')
    if assign_idx < 0 or push_idx < 0:
        fails.append("AC1: overflow record not stamped on gate miss")
    else:
        push_block = ev[assign_idx:push_idx]
        if "wal_append_fail_closed_active()" not in push_block:
            fails.append("AC1: overflow push not gated on fail-closed")
        if "ovr.mid = rec.provenance_mutation_id" not in push_block:
            fails.append("AC1: overflow record missing #3178 join key (mid)")

    # ── AC2: #3493 entry deny unchanged ─────────────────────────────────
    idx3493 = ev.find("if (req_bits != 0 && ::aura::core::wal_slo::wal_append_fail_closed_active() &&")
    if idx3493 < 0:
        fails.append("AC2: #3493 entry deny if missing")
    elif "wal_overflow_ring_full()" not in ev[idx3493 : idx3493 + 400]:
        fails.append("AC2: overflow full missing from entry deny")
    elif "is_strict()" in ev[idx3493 : idx3493 + 400]:
        fails.append("AC2: entry deny ANDs is_strict() (#3493 regression)")
    must("#3109: fail-closed deny", "AC2 #3109 comment marker", ev)

    # ── AC3: Soft fail-open preserved ───────────────────────────────────
    bare = ev.count("(void)g_mutation_audit_wal().append(rec);")
    if bare != 1:
        fails.append(f"AC3: expected exactly 1 bare (void) mutation append (emit observe path), found {bare}")
    must("Issue #3056", "AC3 #3056 cite", ev)

    # ── AC4: FAIL_OPEN opt-out intact ───────────────────────────────────
    must("AURA_WAL_APPEND_FAIL_OPEN", "AC4 opt-out env", slo)
    must('wal_env_flag_truthy("AURA_WAL_APPEND_FAIL_OPEN")', "AC4 OPEN check", slo)

    # ── AC5: #3211 next-outermost gate intact ───────────────────────────
    must("hard-denies the *next* outermost mutate", "AC5 header contract", slo)
    must("check_wal_append_fail_schedule_3211", "AC5 3211 linter wired", build)
    sched = _read("tests/orch/test_security_schedule_gate.cpp")
    if not sched:
        fails.append("AC5: tests/orch/test_security_schedule_gate.cpp missing")

    # ── AC6: test + linter lineage + no-invent ──────────────────────────
    for marker in ("3639 AC1", "3639 AC2", "3639 AC3", "3639 AC4", "3639 AC5"):
        must(marker, f"AC6 test marker {marker}", test)
    must("mutation_wal_append_miss", "AC6 3109 linter carries 3639 rows", lint3109)
    must("mutation_wal_append_miss", "AC6 3302 linter carries 3639 rows", lint3302)
    must("wal_append_missed", "AC6 3109 linter miss-flag row", lint3109)
    must("wal_append_missed", "AC6 3302 linter miss-flag row", lint3302)
    must("check_wal_append_miss_deny_3639", "AC6 build.py wiring", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3639.cpp").is_file():
        fails.append("AC6: test_issue_3639.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3639.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3639.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3639-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3639 same-mutate WAL append-miss fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
