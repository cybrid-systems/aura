#!/usr/bin/env python3
"""Issue #4089: agent-fiber mutate skips the outermost type belt (P0).

Contract (one row per AC):
  AC1  MutationCheckpoint carries the orch_soft_frame marker (default false,
       #4089 cite) so the orch soft checkpoint frame is classifiable
  AC2  orch_soft_boundary_enter stamps cp.orch_soft_frame = true (with cite)
       BEFORE the stack push so a later Guard can classify the frames below
  AC3  Guard ctor flips to the type-authority outermost when on-fiber and
       every frame below is an orch soft frame; the flip happens BEFORE the
       ctor-captured is_outermost_ (#2120) so the existing belt (occurrence
       persist / deferred green proof commit / linear fast-path revalidate)
       and the abort-restore SSOT run; gated on on_fiber (host face
       unchanged); real Guard nesting (any non-soft frame below) stays nested
  AC4  exit_mutation_boundary computes soft_only_below after the pop and the
       nested-success skip fires only for real Guard nesting
       (nested_boundary && success && !soft_only_below); a failed audit on
       the authority boundary takes the restore path (workspace back to the
       pre-mutate tree, success flipped via
       note_outermost_audit_rollback_needs_fail, proof outcome Reject); the
       old unconditional skip is gone
  AC5  build.py wires check_fiber_type_belt_4089 + root allowlist; ACs
       extend tests/serve/test_orch_soft_boundary_unified.cpp; no
       tests/**/test_issue_4089.cpp; no docs/design/4089-*

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

    def forbid(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    ix = _read("src/compiler/evaluator.ixx")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/serve/test_orch_soft_boundary_unified.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: MutationCheckpoint orch_soft_frame marker ───────────────────
    must("bool orch_soft_frame = false;", "AC1 checkpoint marker default false", ix)
    must("Issue #4089", "AC1 evaluator.ixx cites issue", ix)

    # ── AC2: orch_soft_boundary_enter stamps the marker ──────────────────
    must("cp.orch_soft_frame = true;", "AC2 enter stamps orch_soft_frame", efm)
    must("Issue #4089", "AC2 evaluator_fiber_mutation.cpp cites issue", efm)
    stamp_pos = efm.find("cp.orch_soft_frame = true;")
    push_pos = efm.find("active_mutation_stack_static().push_back(std::move(cp))")
    if stamp_pos == -1 or push_pos == -1 or stamp_pos > push_pos:
        fails.append("AC2: marker must be stamped before the stack push")

    # ── AC3: Guard ctor type-authority outermost flip ────────────────────
    must("if (!outermost && on_fiber)", "AC3 flip gated on on_fiber (host face unchanged)", emb)
    must("if (soft_only_below)\n            outermost = true;", "AC3 soft-only-below flip", emb)
    must("bool outermost = (prev == 1);", "AC3 host prev == 1 determination retained", emb)
    flip_pos = emb.find("if (soft_only_below)\n            outermost = true;")
    capture_pos = emb.find("is_outermost_ = outermost;")
    if flip_pos == -1 or capture_pos == -1 or flip_pos > capture_pos:
        fails.append("AC3: flip must precede the ctor-captured is_outermost_ (#2120)")
    must("c4089.orch_soft_frame", "AC3 classifies frames via orch_soft_frame", emb)

    # ── AC4: exit restore on failed authority audit ──────────────────────
    must(
        "if (nested_boundary && success && !soft_only_below)\n"
        "                                goto skip_nested_success_composite_restore;",
        "AC4 nested-success skip only for real Guard nesting",
        emb,
    )
    forbid(
        "if (nested_boundary && success)\n",
        "AC4 old unconditional nested-success skip removed",
        emb,
    )
    must("note_outermost_audit_rollback_needs_fail", "AC4 success flip via existing note", emb)
    must("clear_type_linear_commit_proof_on_abort", "AC4 abort clears proof face", emb)
    must("publish_type_linear_proof_outcome", "AC4 abort publishes Reject outcome", emb)

    # ── AC5: linter wiring + test extension + no new files ───────────────
    must("check_fiber_type_belt_4089", "AC5 build.py wires the linter", build)
    must("check_fiber_type_belt_4089.py", "AC5 root allowlist carries the linter", allow)
    must("Issue #4089", "AC5 test file cites issue", test)
    must("run_test_orch_soft_boundary_unified", "AC5 ACs in soft-boundary batch", test)
    must("--- #4089 AC11", "AC5 AC11 header present", test)
    must("--- #4089 AC13", "AC5 AC13 header present", test)
    if _read("tests/core/test_issue_4089.cpp"):
        fails.append("AC5: tests/core/test_issue_4089.cpp must not exist (#81934)")
    if (ROOT / "docs" / "design").is_dir() and list((ROOT / "docs" / "design").glob("4089-*")):
        fails.append("AC5: docs/design/4089-* must not exist (#1655)")

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("check_fiber_type_belt_4089: OK (5 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
