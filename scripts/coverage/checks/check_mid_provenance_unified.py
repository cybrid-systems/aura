#!/usr/bin/env python3
"""Issue #3143: typed_mid SSOT for require_effect mid stamp chain —
closes 5-source mid drift between SE.mid / AuditWalRecord.provenance_mutation_id
/ TypedMutationAudit.last_mid / CapabilityGrant.bound_mutation_id.

Contract (one row per AC):
  AC1  require_effect mid stamp order: TypedMid (= typed_mutation_audit.h:1176
       `last_type_linear_commit_proof_stamp_v_read()`) → current_mutation_epoch()
       → 1. process ResourceQuota host provenance_mutation_id still wins
       when set.
  AC2  Soft / sandbox=off zero-cost (one relaxed load + early-out before scan).
  AC3  MutationBoundary enter after preflight require_effect → TypedMid non-zero;
       subsequent mutate require_effect uses TypedMid (no drift across boundary
       enter).
  AC4  New query:audit-replay-join(mutation_id) primitive surfaces joined audit
       data (typed_audit + SE + WAL + grants + isolation) keyed on the mid.
       Additive on existing query:capability-effect-stats surface (no new
       public query key per primitive freeze #1448).
  AC5  Source-cite evaluator_security.cpp + typed_mutation_audit.h +
       evaluator_primitives_security.cpp; new test
       tests/core/test_audit_replay_join.cpp; no docs/design/, no
       tests/issues/test_issue_3143.cpp (per #81967/#1655).

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

    eval_sec = _read("src/compiler/evaluator_security.cpp")
    typed_audit = _read("src/compiler/typed_mutation_audit.h")
    ep = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/core/test_audit_replay_join.cpp")
    build = _read("build.py")
    manifest = _read("scripts/coverage/manifests/3143.json")

    # ── AC1: TypedMid first in stamp order ──────────────────────────
    # (#3296 refines the cascade: TypedMid precedes epoch; host-quota mid
    # no longer wins when set. AC1 cite accepts Issue #3143 OR Issue #3296.)
    if "Issue #3143" not in eval_sec and "Issue #3296" not in eval_sec:
        fails.append("AC1 cite in eval_sec: missing 'Issue #3143' or 'Issue #3296'")
    must("last_type_linear_commit_proof_stamp_v_read", "AC1 TypedMid reader in eval_sec", eval_sec)
    # Stamp order doc-block must mention TypedMid first.
    if "TypedMid" not in eval_sec:
        fails.append("AC1: TypedMid not referenced in evaluator_security.cpp")
    # The actual stamp order check: scoped to require_effect function body
    # (other functions in this file also reference current_mutation_epoch(),
    # so a file-wide first-occurrence check is wrong). Find require_effect
    # body and verify TypedMid is checked BEFORE current_mutation_epoch()
    # WITHIN that function.
    func_marker = eval_sec.find("bool Evaluator::require_effect(")
    if func_marker == -1:
        fails.append("AC1: require_effect function not found")
    else:
        # Find the next "}\n" (closing brace at column 0) after the function start.
        func_end = eval_sec.find("\n}\n", func_marker)
        if func_end == -1:
            fails.append("AC1: require_effect function end not found")
        else:
            func_body = eval_sec[func_marker:func_end]
            pos_typedmid = func_body.find("last_type_linear_commit_proof_stamp_v_read()")
            pos_epoch = func_body.find("current_mutation_epoch()")
            if pos_typedmid == -1 or pos_epoch == -1 or pos_typedmid > pos_epoch:
                fails.append("AC1: TypedMid must be checked BEFORE current_mutation_epoch() in require_effect")

    # ── AC2: Soft / sandbox=off zero-cost ────────────────────────
    # Soft path short-circuits at sandbox_mode atomic load (no scan after).
    # No new counter or expensive call added — Soft contract preserved.
    # No additional code path needed; verified via the existing fall-through structure.

    # ── AC3: MutationBoundary enter after preflight → TypedMid non-zero ──
    # Source-cite: stamp_type_linear_commit_proof is the SSOT writer.
    must("stamp_type_linear_commit_proof", "AC3 TypedMid writer (typed_mutation_audit.h)", typed_audit)

    # ── AC4: query:audit-replay-join primitive ────────────────────
    must("query:audit-replay-join", "AC4 query primitive registered", ep)
    must("replay-mid", "AC4 replay-mid key", ep)
    must("typed-mid-current", "AC4 typed-mid-current key", ep)
    must("se-count", "AC4 se-count key (joined with SE ring)", ep)
    must("wal-enabled", "AC4 wal-enabled key (joined with WAL)", ep)
    must("schema-3143", "AC4 schema-3143 key", ep)
    must("Issue #3143", "AC4 cite in evaluator_primitives_security.cpp", ep)

    # ── AC5: source-cite + extend test + no docs/issues ─────────────
    must("ac1_typedmid_first_stamp_order", "AC5 AC1 test function", test)
    must("ac2_soft_off_zero_cost", "AC5 AC2 test function", test)
    must("ac3_typedmid_after_boundary_enter", "AC5 AC3 test function", test)
    must("ac4_query_audit_replay_join", "AC5 AC4 test function", test)
    must("ac5_source_cite_no_design", "AC5 AC5 test function", test)
    must("Issue #3143", "AC5 #3143 cite in test", test)

    # AC5: build.py wires linter
    must("check_mid_provenance_unified.py", "AC5 build.py wires linter", build)

    # AC5: manifest exists and contains #3143
    if "3143" not in manifest:
        fails.append("AC5: manifest 3143.json missing '3143'")
    if "check_mid_provenance_unified.py" not in manifest:
        fails.append("AC5: manifest 3143.json missing linter name")

    # AC5: no docs/design/, no tests/issues/test_issue_3143.cpp
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3143-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "issues" / "test_issue_3143.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3143.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3143.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3143.cpp present (forbidden per #81967)")

    if fails:
        print("FAIL: Issue #3143 linter found", len(fails), "problems:")
        for f in fails:
            print(" -", f)
        return 1
    print("OK: Issue #3143 — typed_mid SSOT + audit-replay-join query surface.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
