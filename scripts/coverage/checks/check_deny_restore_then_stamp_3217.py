#!/usr/bin/env python3
"""Issue #3217: deny path restores before trail/SE stamp.

Main hard-gate already restores then stamps. Residual is a unified
order invariant across composite / nested / lockless-child / linear-synth
/ densify force / AOT fail, plus production mid=0 never invents Success.

Contract (one row per AC):
  AC1  Header + boundary document 1 restore / 2 clear / 3 stamp;
       record_boundary_deny_after_restore helper
  AC2  Bypass sites: invariant-force-rollback, linear-synth, rollback,
       AOT fail (Error after swap refused)
  AC3  schema-3217 on query:typed-mutation-audit-trail (no new query:*)
  AC4  production mid=0 Success skipped; capture_audit_event_forced drops 0
  AC5  Soft: helper is record_boundary_outcome(success=false) — no extra I/O
  AC6  test_hard_gate_full_strict ac3217_*; linter in build.py;
       no test_issue_3217.cpp; no docs/design/3217-*

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

    aud = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    jit = _read("src/compiler/aura_jit_bridge.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    t = _read("tests/compiler/test_hard_gate_full_strict.cpp")
    build = _read("build.py")

    must("kDenyRestoreThenStampIssue = 3217", "AC1 constant", aud)
    must("record_boundary_deny_after_restore", "AC1 helper", aud)
    must("1. structural restore", "AC1 order step 1", aud)
    must("Issue #3217 deny-path stamp order", "AC1 boundary docs", mb)
    must("THEN record_boundary_deny_after_restore", "AC1 step 3", mb)

    must("record_boundary_deny_after_restore", "AC2 helper used", mb)
    must("composite-invariant-force-rollback", "AC2 composite deny", mb)
    must("linear-synth-hard-fail", "AC2 linear-synth", mb)
    must("clear_type_linear_commit_proof_on_abort", "AC2 proof clear", mb)
    must("never Success-then-rollback", "AC2 AOT fail order", jit)
    if "query:deny-restore" in mut or "query:deny-restore-then-stamp" in mut:
        fails.append("AC3: new query:* name (reuse query:typed-mutation-audit-trail)")

    must("schema-3217", "AC3 schema", mut)
    must("deny-restore-then-stamp-wired", "AC3 wired", mut)

    must("if (outcome == AuditOutcome::Success)", "AC4 skip Success", aud)
    must("capture_audit_event_forced(0,", "AC4 drop mid=0", aud)
    must("if (mutation_id == 0)", "AC4 forced drop", aud)

    must(
        "record_boundary_outcome(mutation_id, op, before_epoch, after_epoch, /*success=*/false",
        "AC5 helper delegates, no extra I/O",
        aud,
    )

    must("ac3217_deny_restore_then_stamp", "AC6 test fn", t)
    must("ac3217_4: production mid=0 does not stamp fake Success", "AC6 mid=0", t)
    must("ac3217_5: Soft/Off deny helper does not force extra trail I/O", "AC6 Soft", t)
    must("check_deny_restore_then_stamp_3217", "AC6 build.py", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3217.cpp").is_file():
        fails.append("AC6: test_issue_3217.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3217.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3217.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3217-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3217 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3217 deny restore-then-stamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
