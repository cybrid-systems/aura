#!/usr/bin/env python3
"""Issue #3193: nested abort + concurrent densify/steal one authority face.

Under nested MutationBoundary + concurrent densify/steal, dual_clear
coercion + clear occurrence persist + proof invalidate can interleave
with rehydrate. Production/Full publishes AbortAuthorityHold (reuses
g_rehydrate_miss_invalidate_gen) BEFORE topology restore so observers
cannot mix CoercionMap / persist / TypeLinearCommitProof. Soft: observe
only. Quiet (no abort): zero extra.

Contract:
  AC1 Production nested abort + concurrent densify/steal → post-window
      persist matches authority, proof Reject, rehydrate blocked during hold
  AC2 No mixed green proof + residual persist/coercion visible to rehydrate
  AC3 Soft observe-only; quiet path unchanged
  AC4 Extend persist-rehydrate suite; coverage linter; no docs/design / invent

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
    impl = _read("src/compiler/type_checker_impl.cpp")
    qs = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    build = _read("build.py")

    # AC1 — hold helper + abort sites + rehydrate consult.
    must("kNestedAbortAuthorityFaceIssue", "AC1", aud)
    must("begin_abort_authority_hold", "AC1", aud)
    must("end_abort_authority_hold", "AC1", aud)
    must("AbortAuthorityHold", "AC1", aud)
    must("g_rehydrate_miss_invalidate_gen.fetch_add", "AC1 reuse gen", aud)
    must("AbortAuthorityHold abort_authority", "AC1 abort sites", mb)
    if mb.count("AbortAuthorityHold abort_authority") < 3:
        fails.append("AC1: expected ≥3 AbortAuthorityHold sites in mutation_boundary")
    if mb.count("aura_clear_occurrence_persist_buffer(this)") < 3:
        fails.append("AC1: expected ≥3 persist-clear-on-abort-body sites")
    must("abort_authority_blocks_rehydrate", "AC1 rehydrate consult", impl)
    must("Issue #3193", "AC1 rehydrate cite", impl)

    # AC2 — no mixed face: rehydrate returns 0 while in_flight.
    must("abort_authority_blocks_rehydrate()", "AC2", impl)
    must("return 0;", "AC2 skip", impl)
    must("ac3193_2_no_mixed_green_residual", "AC2 test", t)

    # AC3 — Soft observe; quiet ctor not on success path.
    must("g_abort_authority_hold_observe_total", "AC3", aud)
    must("observe-only", "AC3", aud)
    must("ac3193_3_soft_observe_quiet_zero", "AC3 test", t)

    # AC4 — extend existing suite; linter; no invent.
    must("ac3193_1_prod_hold_blocks_rehydrate", "AC4", t)
    must("ac3193_4_source_and_linter", "AC4", t)
    must("schema-3193", "AC4 schema", qs)
    must("check_nested_abort_authority_face_3193", "AC4 build.py", build)
    must("Issue #3030", "AC4 sibling proof-clear", aud)
    must("dual_clear_coercion_state_on_abort", "AC4 sibling 3116", mb)
    must("clear_type_linear_commit_proof_on_abort", "AC4 sibling 3030", mb)

    if (ROOT / "tests" / "issues" / "test_issue_3193.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3193.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3193.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3193.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3193-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3193 nested_abort_authority_face:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3193 nested_abort_authority_face: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
