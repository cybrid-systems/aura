#!/usr/bin/env python3
"""Issue #3655: persist requires this-boundary SDO for non-rebind mutate.

rebind/set-body already run post-mutate typecheck + finish_mutate_hard_gate.
replace-subtree / tweak-literal / lockless eval_flat_apply_* did not, so
expected_fp==0 && live==0 froze an empty Occurrence snapshot.

Contract (one row per AC):
  AC1  Production/Full + mutated + !staged → run_post_mutate_typecheck
       before outermost persist
  AC2  rebind/set-body finish_mutate_hard_gate still present
  AC3  vacuous (no dirty/log delta) skips extra SDO
  AC4  Soft does not force extra SDO
  AC5  extends persist-rehydrate + typed_mutate_incremental_gaps;
       linter AFTER #3654; no test_issue_3655.cpp; no docs/design/;
       no query:type-linear-* rewrite

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

    dtor = _read("src/compiler/evaluator_mutation_boundary.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    occ = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    gaps = _read("tests/compiler/test_typesystem_typed_mutate_incremental_gaps.cpp")
    build = _read("build.py")

    win_at = dtor.find("Issue #3655")
    win = dtor[win_at : win_at + 2200] if win_at >= 0 else ""

    must("Issue #3655", "AC1 Guard cite", dtor)
    must("occurrence_fp_staged()", "AC1 staged gate", win)
    must("run_post_mutate_typecheck_no_lock()", "AC1 persist-front typecheck", win)
    must("expected_occurrence_fp_staged_", "AC1 staged field", ixx)
    must("stage_fp_if_solved", "AC1 typecheck stages empty SOLVED", tc)

    must("finish_mutate_hard_gate", "AC2 hard-gate", mut)
    must("mutate:rebind", "AC2 rebind", mut)
    must("mutate:set-body", "AC2 set-body", mut)

    if "mark_dirty_upward_call_count()" not in win and "mutation_log_size()" not in win:
        fails.append("AC3: vacuous skip missing dirty/log delta")

    must("production_defaults_active()", "AC4 Soft skip", win)

    must("check_persist_sdo_before_unstaged_3655", "AC5 build.py", build)
    must("ac3655_persist_requires_this_mutate_sdo", "AC5 persist-rehydrate", occ)
    must("ac3655_non_rebind_persist_sdo", "AC5 gaps suite", gaps)
    prev = build.find("check_jit_linear_post_mutate_unset_3654")
    ours = build.find("check_persist_sdo_before_unstaged_3655")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3654")
    if "query:type-linear-commit-health" in win or "query:type-linear-evolution-snapshot" in win:
        fails.append("AC5: rewrote old query:type-linear-* key")
    if _read("tests/compiler/test_issue_3655.cpp"):
        fails.append("AC5: test_issue_3655.cpp present")
    if _read("docs/design/3655-persist-sdo.md"):
        fails.append("AC5: docs/design/ exists")

    if fails:
        print("FAIL #3655 persist_sdo_before_unstaged:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3655 persist_sdo_before_unstaged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
