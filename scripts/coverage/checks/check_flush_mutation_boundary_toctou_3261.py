#!/usr/bin/env python3
"""Issue #3261: flush_mutation_boundary outermost TOCTOU + hygiene hoist.

Sample mutation depth once (TLS / per-fiber stack, not a cross-thread
atomic). Drop the dead `|| depth==1` re-read. Hoist
hygiene_violation_prevented_on_boundary_total off mark_all_defines_dirty_fn_
/ compiler_metrics_ so an unwired dirty-propagation pointer cannot silently
drop the event. Keep #3260 file-level dual-write. Soft empty-stack path
unchanged (early return).

Contract:
  AC1  depth sampled once; no dead || mutation_boundary_depth() re-read
  AC2  hygiene bump outside dirty-fn / inner metrics if; dual-write kept
  AC3  empty stack still early-return (zero extra)
  AC4  #3260 file-level accessor still next to per-eval bump
  AC5  extend test_clone_provenance_per_evaluator; linter after #3260; no invent

Exit 0 = all rows satisfied.

Follow-up #3262: GC safepoint restamp-after-unlock + audit acquire-load.

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

    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_clone_provenance_per_evaluator.cpp")
    build = _read("build.py")
    l3260 = _read("scripts/coverage/checks/check_macro_provenance_counter_unify_3260.py")

    pos = fiber.find("void Evaluator::flush_mutation_boundary()")
    win = fiber[pos : pos + 3200] if pos >= 0 else ""
    must("Issue #3261", "AC1 cite", win)
    must("const auto depth = stack.size()", "AC1 once", win)
    must("if (outermost_active)", "AC1 if", win)
    if "if (outermost_active ||" in win:
        fails.append("AC1: dead || depth re-read still present")
    if "|| mutation_boundary_depth()" in win:
        fails.append("AC1: second mutation_boundary_depth() in flush if")
    must("TLS", "AC1 TLS document", win)
    must("ac3261_1_depth_sampled_once", "AC1 test", test)

    dirty = win.find("if (mark_all_defines_dirty_fn_)")
    hyg = win.find("Issue #1908 / #3260 / #3261")
    if dirty < 0 or hyg < 0 or hyg < dirty:
        fails.append("AC2: hygiene hoist must follow dirty-fn block")
    else:
        dblock = win[dirty:hyg]
        if "bump_hygiene_violation_prevented_on_boundary_total" in dblock:
            fails.append("AC2: hygiene bump still inside dirty-fn gate")
    must("bump_hygiene_violation_prevented_on_boundary_total()", "AC2 per-eval", win)
    must("aura_bump_hygiene_violation_prevented_on_boundary_total(1)", "AC2 file-level", win)
    must("ac3261_2_hygiene_hoisted_off_dirty_fn", "AC2 test", test)

    must("if (stack.empty())", "AC3 empty", win)
    must("ac3261_3_empty_stack_zero_extra", "AC3 test", test)

    must("aura_bump_hygiene_violation_prevented_on_boundary_total(1)", "AC4 dual-write", win)
    if "aura_bump_macro_provenance_repin_on_steal_total(1)" in win:
        fails.append("AC4: flush must not dual-write repin (hygiene-only site)")

    must("ac3261_5_source_and_linter", "AC5 test", test)
    must("check_flush_mutation_boundary_toctou_3261", "AC5 build.py", build)
    must("Issue #3261", "AC5 evaluator.ixx", ixx)
    prev = build.find("check_macro_provenance_counter_unify_3260")
    ours = build.find("check_flush_mutation_boundary_toctou_3261")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3260")
    must("3261", "AC5 extend 3260 linter", l3260)
    if (ROOT / "tests" / "issues" / "test_issue_3261.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3261.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3261.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3261.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3261-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3261" in q or "schema-3261" in test:
        fails.append("AC5: new schema-3261 query key (SlimSurface)")

    if fails:
        print("FAIL #3261 flush_mutation_boundary_toctou:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3261 flush_mutation_boundary_toctou: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
