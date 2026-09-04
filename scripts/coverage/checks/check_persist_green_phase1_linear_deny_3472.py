#!/usr/bin/env python3
"""Issue #3472: persist-green then Phase-1 linear deny must flip success.

#3440 closed persist-reject restore. Persist helper still stamps green
before Phase-1, and (void)enforce discarded the linear deny. Production
now walks enforce before exit_mutation_boundary and flips success into
the existing abort_restore SSOT when !all_safe or synth pending.

Contract:
  AC1 persist → consume → enforce+pending check → flip → exit
  AC2 #3440 persist-reject unchanged (no second restore; 3 #3158 sites)
  AC3 happy persist + linear_ok: no extra abort helper
  AC4 Soft/Off gated on production_defaults / Full; no new query key
  AC5 extend the three named suites; no test_issue_3472.cpp

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t_lin = _read("tests/compiler/test_linear_enforce_production_defaults.cpp")
    t_health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    t_persist = _read("tests/compiler/test_outermost_persist_fail_closed.cpp")

    persist_call = emb.find("aura_outermost_success_persist_occurrence(ev_")
    consume = emb.find("consume_outermost_persist_reject_needs_restore()")
    issue = emb.find("Issue #3472")
    exit_pos = emb.find("ev_->exit_mutation_boundary(success)")
    if persist_call < 0 or consume < 0 or issue < 0 or exit_pos < 0 or not (persist_call < consume < issue < exit_pos):
        fails.append("AC1: persist → consume → #3472 → exit_mutation_boundary ordering missing")

    win = emb[issue:exit_pos] if issue >= 0 and exit_pos > issue else ""
    must("enforce_linear_boundary_consistency", "AC1 pre-exit enforce", win)
    must("linear_synth_hard_fail_pending", "AC1 pending deny", win)
    must("clear_type_linear_commit_proof_on_abort", "AC1 #3030 clear", win)
    must("aura_clear_occurrence_persist_buffer", "AC1 persist clear", win)
    must("clear_type_export_authority", "AC1 grant drop", win)
    must("success = false", "AC1 flip", win)
    must("production_defaults_active()", "AC4 hard gate", win)
    must("AuditStrategy::Full", "AC4 Full", win)
    must_not("linear_post_mutate_force_rollback_total", "AC1 rollback counter not deny", win)

    must_not("abort_restore_dual_topology_3472", "AC2 no second restore", emb)
    must_not("abort_restore_dual_topology_persist_reject", "AC2 no persist-reject restore", emb)
    occ_n = emb.count("restore_or_clear_occurrence_to_entry(")
    if occ_n != 3:
        fails.append(f"AC2: #3158 abort sites count={occ_n}, expected 3 (reuse SSOT)")
    must("consume_outermost_persist_reject_needs_restore()", "AC2 consume kept", emb)

    must("3472 AC1: success==false", "AC5 linear AC1", t_lin)
    must("3472 AC3: no extra abort", "AC5 linear AC3", t_lin)
    must("3472 AC4: Soft does not hard-flip success", "AC5 linear AC4", t_lin)
    must("3472 AC1: persist-green × Phase-1 pending → IR refused", "AC5 health AC1", t_health)
    must("3472 AC2: persist → consume → #3472 check → exit", "AC5 health AC2", t_health)
    must("3472 live: success==false (not only contains(src))", "AC5 persist live", t_persist)

    must_not("schema-3472", "AC4 no query key", emb)
    must_not("g_3472_", "AC4 no new counter", emb)

    if (ROOT / "tests" / "compiler" / "test_issue_3472.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3472.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3472.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3472.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3472-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3472 persist_green_phase1_linear_deny:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3472 persist_green_phase1_linear_deny: persist-green × linear deny flips success")
    return 0


if __name__ == "__main__":
    sys.exit(main())
