#!/usr/bin/env python3
"""Issue #4151 source-cite gate: expand_inner_macros non-macro child
walk must carry the in-function production gate.

expand_inner_macros refused to splice a half-expanded MACRO-call clone
under production (#3684/#3890 truncate_to + optional ckpt restore), and
the outer callers (macro_expand_all_body, eval_flat expand,
post_mutation_macro_reexpand) belted with
inner_expand_production_limit_deny(_all) + checkpoint restore — so the
production closed loop held only through the callers. The NON-MACRO
CHILD RECURSE path walked siblings with (void)expand_inner_macros(...)
and returned the parent root WITHOUT consulting the deny or truncating
earlier successful sibling expansions: any standalone caller (Agent /
self-evo / reexpand) could observe a sibling half-expand after a later
child hit depth/pass/gensym/steal/cap deny.

ACs:
  AC1  The non-macro child walk captures a pre-walk checkpoint and, on
       production_surface && inner_expand_production_limit_deny(),
       truncates back to it and returns the original root.
  AC2  The gate restores the snapshotted child edges (set_child from
       the pre-walk child_ids snapshot) before truncating — no dangling
       spliced clone NodeIds.
  AC3  Soft/Off contract kept: the gate is production-gated and the
       historical partial-expand comment survives (no new sandbox
       mode / no second surface).
  AC4  No second model: the gate mirrors the #4077 owned-checkpoint
       guard (expand_inner_checkpoint_owned + try_restore), and the
       caller belts stay — macro_expand_all_body's deny_all consult and
       eval_flat's production consults are unchanged.
  AC5  Tests extended (ac4151_* in test_macro_hygiene_limits.cpp); no
       test_issue_4151.cpp; no docs/design/4151-*; linter on the root
       allowlist.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ME = ROOT / "src" / "compiler" / "macro_expansion.cpp"
FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
LIM = ROOT / "tests" / "compiler" / "test_macro_hygiene_limits.cpp"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def main() -> int:
    me = ME.read_text() if ME.exists() else ""
    flat = FLAT.read_text() if FLAT.exists() else ""
    lim = LIM.read_text() if LIM.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    good = (
        "Issue #4151" in me
        and "const std::size_t child_ckpt = flat->size();" in me
        and "flat->truncate_to(child_ckpt)" in me
    )
    report("AC1", good, "child walk deny arm truncates to the pre-walk checkpoint")

    good = "current_children" in me and "child_ids[ci]" in me and "restore_n" in me
    report("AC2", good, "gate restores the snapshotted child edges (set_child)")

    good = (
        "production_surface && inner_expand_production_limit_deny()" in me
        and "Soft/Off keeps the" in me
        and "historical partial expand" in me
    )
    report("AC3", good, "Soft/Off keeps the historical partial expand (contract)")

    good = (
        "expand_inner_checkpoint_owned()" in me
        and "inner_expand_production_limit_deny_all()" in me
        and "inner_expand_production_limit_deny()" in flat
        and "aura::core::sandbox::is_sandbox_active()" in flat
    )
    report("AC4", good, "no second model; caller belts unchanged")

    good = (
        "ac4151_sibling_deny_rolls_back_splice" in lim
        and "ac4151_nested_cascade_rolls_back" in lim
        and "ac4151_soft_keeps_sibling_splice" in lim
        and not (ROOT / "tests" / "compiler" / "test_issue_4151.cpp").exists()
        and not (ROOT / "docs" / "design" / "4151-macro-child-walk-gate.md").exists()
        and "check_macro_child_walk_gate_4151.py" in allow
    )
    report("AC5", good, "tests extended; no new artifacts; linter allowlisted")

    print("Issue #4151 macro child walk gate linter: " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
