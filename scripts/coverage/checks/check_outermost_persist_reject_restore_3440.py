#!/usr/bin/env python3
"""Issue #3440: outermost persist-reject must abort_restore the already-success AST.

#3376 closed the authority face (Reject stamp / no grant). Persist-reject
still ran under outermost && success AFTER exit_mutation_boundary, so the
mutated FlatAST + CoercionMap inserts stayed live. Production/Full now
notes a TLS restore flag on every persist-reject arm and consumes it
BEFORE exit_mutation_boundary, flipping success so the existing !success
abort_restore SSOT (#3030/#3102/#3158) runs. Soft/Off note is a no-op.
No new query key / no second restore / no g_3440_* counter.

Contract:
  AC1 Production persist-reject notes restore (all reject arms)
  AC2 dtor persist → consume → flip success before exit_mutation_boundary
  AC3 abort_restore stays SSOT (exactly 3 #3158 sites; no second restore)
  AC4 Soft/Off: note gated on production_defaults / Full
  AC5 no docs/design/*; no test_issue_*.cpp; no schema-3440

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    tma = _read("src/compiler/typed_mutation_audit.h")
    t_abort = _read("tests/compiler/test_occurrence_abort_restore.cpp")
    t_persist = _read("tests/compiler/test_outermost_persist_fail_closed.cpp")
    t_rehy = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    t_light = _read("tests/compiler/test_type_linear_lightweight_abort_clear.cpp")
    t_cone = _read("tests/compiler/test_partial_cone_commit_gate.cpp")
    build = _read("build.py")

    must("kOutermostPersistRejectRestoreIssue = 3440", "AC1 stamp", tma)
    must("note_outermost_persist_reject_needs_restore", "AC1 note", tma)
    must("g_tls_outermost_persist_reject_needs_restore", "AC1 TLS flag", tma)
    must("note_3440_restore()", "AC1 helper lambda", emb)
    start = emb.find('extern "C" void aura_outermost_success_persist_occurrence(')
    end = emb.find('extern "C" void aura_clear_occurrence_persist_buffer', start)
    helper = emb[start:end] if start >= 0 and end > start else ""
    n = helper.count("note_3440_restore()")
    if n < 9:
        fails.append(f"AC1: persist-reject arms note restore count={n}, expected >= 9")
    must("3440 AC1", "AC1 test abort", t_abort)
    must("note_3440_restore()", "AC1 test persist", t_persist)

    persist_call = emb.find("aura_outermost_success_persist_occurrence(ev_")
    consume = emb.find("consume_outermost_persist_reject_needs_restore()")
    exit_pos = emb.find("ev_->exit_mutation_boundary(success)")
    if persist_call < 0 or consume < 0 or exit_pos < 0 or not (persist_call < consume < exit_pos):
        fails.append("AC2: persist → consume → exit_mutation_boundary ordering missing")
    must("Issue #3440", "AC2 cite", emb)
    must("success = false", "AC2 flip", emb[consume : consume + 400] if consume >= 0 else "")
    must("3440 AC2", "AC2 test", t_abort)

    if "abort_restore_dual_topology_persist_reject" in emb:
        fails.append("AC3: second persist-reject restore helper invented")
    occ_n = emb.count("restore_or_clear_occurrence_to_entry(")
    if occ_n != 3:
        fails.append(f"AC3: #3158 abort sites count={occ_n}, expected 3 (reuse SSOT)")
    must("3440 AC3", "AC3 test", t_abort)
    must("abort_restore_dual_topology", "AC3 cone test", t_cone)

    note_fn = tma.find("inline void note_outermost_persist_reject_needs_restore")
    note_body = tma[note_fn : note_fn + 500] if note_fn >= 0 else ""
    must("production_defaults_active()", "AC4 hard gate", note_body)
    must("AuditStrategy::Full", "AC4 Full", note_body)
    must("3440 AC4", "AC4 test", t_abort)

    must("check_outermost_persist_reject_restore_3440", "AC5 build.py", build)
    must("3440 AC5", "AC5 test", t_abort)
    must("Issue #3440", "AC5 rehydrate", t_rehy)
    must("Issue #3440", "AC5 lightweight", t_light)
    if "g_3440_" in emb or "g_3440_" in tma:
        fails.append("AC5: new g_3440_* counter")
    if "schema-3440" in emb or "schema-3440" in tma:
        fails.append("AC5: new schema-3440 query key")
    if (ROOT / "tests" / "compiler" / "test_issue_3440.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3440.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3440.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3440.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3440-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3440 outermost_persist_reject_restore:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3440 outermost_persist_reject_restore: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
