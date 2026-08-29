#!/usr/bin/env python3
"""Issue #3347: residual CastOp undermark before commit_readiness / grant.

#3228 remirrors persist on selective dirty-txn + composite_txn_commit.
Single-boundary / lockless paths that evaluate commit_readiness without
those call sites could observe an empty cone and grant. Production
live_policy + grant remirror via C ABI before auto_partial / empty_cs /
type authority. Soft / empty persist: 0 extra. Reuses #3065/#3228.

Contract:
  AC1 live_policy remirror before auto_partial; grant remirror + pending
  AC2 pending latch until infer; cone nonempty or hard-reject
  AC3 C ABI n>0 bumps decision invalidate; post-infer remirror does not
  AC4 Soft/quiet 0 extra; after #3228; no invent / docs/design /
      g_3347_* / schema-3347

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

    dirty = _read("src/compiler/dirty_propagation.ixx")
    tma = _read("src/compiler/typed_mutation_audit.h")
    ev = _read("src/compiler/evaluator.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    stubs = _read("src/compiler/test_concurrent_stubs.cpp")
    t = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    col = _read("tests/compiler/test_dead_coercion_columnar.cpp")
    batch = _read("tests/compiler/test_batch_dirty_cascade.cpp")
    inc = _read("tests/compiler/test_incremental_type_batch.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kResidualCastopReadinessUndermarkIssue = 3347", "AC1 stamp", dirty)
    must("aura_force_residual_castop_undermark_into_cone", "AC1 C ABI def", dirty)
    must("note_residual_castop_undermark_pending", "AC1 latch", dirty)
    must("aura_force_residual_castop_undermark_into_cone", "AC1 C ABI decl", tma)
    must("aura_residual_castop_undermark_pending", "AC1 pending decl", tma)
    must("ac3347_1_live_policy_remirrors_before_auto_partial", "AC1 test", t)

    live_idx = tma.find("inline CommitReadinessInput commit_readiness_live_policy() noexcept {")
    fill_idx = tma.find("aura_typed_audit_fill_from_live_tc", live_idx if live_idx >= 0 else 0)
    force_idx = tma.find("aura_force_residual_castop_undermark_into_cone", live_idx if live_idx >= 0 else 0)
    if live_idx < 0:
        fails.append("AC1: commit_readiness_live_policy definition missing")
    elif force_idx < 0 or force_idx > live_idx + 2500:
        fails.append("AC1: live_policy does not call force remirror")
    elif fill_idx < 0 or force_idx > fill_idx:
        fails.append("AC1: live_policy remirror must precede fill_from_live_tc")
    must("auto_partial_from_cone = true", "AC1 pending → auto_partial", tma)

    must("aura_force_residual_castop_undermark_into_cone", "AC1 grant remirror", ev)
    must("aura_residual_castop_undermark_pending", "AC1 grant refuse", ev)
    grant_idx = ev.find("void grant_type_export_authority() noexcept")
    gforce_idx = ev.find("aura_force_residual_castop_undermark_into_cone", grant_idx if grant_idx >= 0 else 0)
    if grant_idx < 0 or gforce_idx < 0 or gforce_idx > grant_idx + 1200:
        fails.append("AC1: grant_type_export_authority does not remirror")

    must("clear_residual_castop_undermark_pending", "AC2 infer clear", impl)
    must("ac3347_2_soft_quiet", "AC2/AC4 test", t)
    if impl.count("clear_residual_castop_undermark_pending") < 2:
        fails.append("AC2: expected clear on empty-affected + post-infer remirror")

    must("note_residual_castop_undermark_pending", "AC3 bump site", dirty)
    must("bump_dead_coercion_decision_invalidate", "AC3 abort bump retained", dirty)
    must("ac3347_3_invalidate_gen_success_path", "AC3 test", t)
    if "schema-3347" in q:
        fails.append("AC3: new schema-3347 query key")
    if "g_3347_" in dirty or "g_3347_" in tma:
        fails.append("AC3: new g_3347_* counter")

    must("if (!residual_castop_persist_active())", "AC4 persist gate", dirty)
    must("aura_force_residual_castop_undermark_into_cone", "AC4 light-link stub", stubs)
    must("check_residual_castop_readiness_undermark_3347", "AC4 build.py", build)
    must("check_residual_castop_undermark_cone_3228", "AC4 after #3228", build)
    # #3347 gate must appear after #3228 in build.py.
    i3228 = build.find("check_residual_castop_undermark_cone_3228")
    i3347 = build.find("check_residual_castop_readiness_undermark_3347")
    if i3228 < 0 or i3347 < 0 or i3347 < i3228:
        fails.append("AC4: #3347 linter must run after #3228")
    must("3347", "AC4 columnar", col)
    must("3347", "AC4 cascade", batch)
    must("3347", "AC4 incremental_type", inc)
    must("ac3347_4_linter_no_invent", "AC4 test", t)
    if (ROOT / "tests" / "issues" / "test_issue_3347.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3347.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3347.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3347.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3347-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3347 residual_castop_readiness_undermark:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3347 residual_castop_readiness_undermark: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
