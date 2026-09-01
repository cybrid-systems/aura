#!/usr/bin/env python3
"""Issue #3335: emit_mutation_audit stamps Mutation epoch, not Bridge.

#2149 unified grant / check_and_record_effect / SE to WorkspaceEpoch
Mutation. Residual: Evaluator::emit_mutation_audit still wrote
slot.epoch = current_bridge_epoch(), so the mutation audit ring could
not join grant.bound_mutation_id / SE.mutation_id / TypedMid.

Contract (one row per AC):
  AC1  emit_mutation_audit slot.epoch from current_mutation_epoch()
       (#3462 update: production/Full refuse (mid==0) writes NO row at
       all and slot.epoch stays raw (never coerced to 1); Soft/Off
       keeps the legacy last-resort non-zero stamp)
  AC2  Bridge-only bump does not change ring epoch vs grant
  AC3  schema-2055 / audit stay green; ring epoch == Mutation after grant+mutate
  AC4  Soft/Off ring write still happens (epoch source only)
  AC5  ring.epoch + SE.mutation_id + grant.bound_mutation_id same vocabulary
  AC6  extend test_effect_epoch_mutation_unify; linter after #3296;
       no invent / no docs/design

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

    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    cap = _read("src/core/capability_model.hh")
    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    wal = _read("src/core/mutation_audit_wal.hh")
    test = _read("tests/compiler/test_effect_epoch_mutation_unify.cpp")
    lint3296 = _read("scripts/coverage/checks/check_require_effect_mid_ssot_3296.py")
    build = _read("build.py")

    emit_pos = sec.find("void Evaluator::emit_mutation_audit")
    if emit_pos < 0:
        fails.append("AC1: emit_mutation_audit missing")
        emit_body = ""
    else:
        nxt = sec.find("\nbool Evaluator::", emit_pos)
        if nxt < 0:
            nxt = sec.find("\nvoid Evaluator::", emit_pos + 10)
        emit_body = sec[emit_pos : nxt if nxt > 0 else emit_pos + 2500]
    must("current_mutation_epoch()", "AC1 emit Mutation stamp", emit_body)
    must("Issue #3335", "AC1 cite", emit_body)
    if "slot.epoch = current_bridge_epoch()" in emit_body:
        fails.append("AC1: emit_mutation_audit still stamps Bridge as slot.epoch")
    # Issue #3462: production refuse joins the mid SSOT and writes no row
    # (no phantom mid|epoch=1); the coercion literal survives ONLY in the
    # Soft/Off legacy stamp branch, never on slot.epoch directly.
    must("join_audit_and_se_mid(0)", "AC1 production mid SSOT", emit_body)
    if "slot.epoch = me != 0 ? me : 1" in emit_body:
        fails.append("AC1: slot.epoch coerced to 1 (#3462: raw epoch under production)")
    if "epoch = me != 0 ? me : 1" not in emit_body:
        fails.append("AC1: Soft/Off legacy stamp missing (#3462)")

    must("ac3335_1_emit_stamps_mutation", "AC1 test", test)
    must("kMutationAuditEpochUnifyIssue = 3335", "AC1 stamp", cap)

    must("bridge_epoch = 0", "AC2 END-append field", ixx)
    entry_pos = ixx.find("struct MutationAuditEntry")
    if entry_pos < 0:
        fails.append("AC2: MutationAuditEntry missing")
    else:
        entry = ixx[entry_pos : entry_pos + 1200]
        epoch_pos = entry.find("std::uint64_t epoch = 0;")
        be_pos = entry.find("std::uint64_t bridge_epoch = 0;")
        if be_pos < 0 or epoch_pos < 0 or be_pos < epoch_pos:
            fails.append("AC2: bridge_epoch must END-append after epoch (#2906)")
    must("ac3335_2_bridge_bump_no_ring_flip", "AC2 test", test)
    if "slot.epoch = current_bridge_epoch()" in emit_body:
        fails.append("AC2: Bridge must not be the ring join key")

    must("ac3335_3_grant_mutate_ring_epoch", "AC3 test", test)
    must("schema-2055", "AC3 schema-2055 retained", prim)
    must("schema-3335", "AC3 additive schema", prim)
    must("sizeof(AuditWalRecord) == 8 + 8 + 8 + 4 + 4 + 4 + 48 + 2 + 2 + 8 + 8 + 8 + 1 + 7", "AC3 WAL size stable", wal)

    must("ac3335_4_soft_off_write", "AC4 test", test)
    must("Soft/Off: relaxed loads only", "AC4 zero extra cost", emit_body)

    must("ac3335_5_agent_join_vocab", "AC5 test", test)
    must("ring.epoch + SE.mutation_id + grant.bound_mutation_id", "AC5 join sentence", test)
    must("last_type_linear_commit_proof_stamp_v_read()", "AC5 TypedMid mid fill", emit_body)
    must("mutation-audit-epoch-mutation-wired", "AC5 wired key", prim)

    must("check_mutation_audit_epoch_3335", "AC6 build.py", build)
    must("check_require_effect_mid_ssot_3296", "AC6 #3296 linter wired", build)
    must("ac3335_6_source_and_linter", "AC6 test", test)
    must("Issue #3296", "AC6 3296 linter retained", lint3296)
    prev = build.find("check_require_effect_mid_ssot_3296")
    ours = build.find("check_mutation_audit_epoch_3335")
    if ours < 0:
        fails.append("AC6: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC6: linter must be wired in build.py AFTER #3296")
    if (ROOT / "tests" / "compiler" / "test_issue_3335.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3335.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3335-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3335 mutation audit ring epoch = Mutation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
