#!/usr/bin/env python3
"""Issue #3858 source-cite gate: restore-hygiene-checkpoint MSE demote gate.

#3344 marked mutate:restore-hygiene-checkpoint HYGIENE_EXEMPT, but the
restore reinstalls the checkpointed marker / provenance / macro_dirty
columns with no MSE consult. A checkpoint taken before an expand can
demote live MacroIntroduced → User on restore, vacating the
#3344/#3542 structural default-deny without MacroSelfEvo. Parallel
unstamp faces (rollback-macro-introduced, syntax:set-marker clear)
already require MSE under Restricted (#3650) — restore does not.

ACs:
  AC1  the prim gates under sandbox: peek restore_would_demote_macro_
       introduced(handle) → deny_macro_opt_out_without_mse(ev, demote_
       id, mev) → ok=false deny, BEFORE restore_hygiene_checkpoint_
       handle(handle) consumes the handle.
  AC2  the Evaluator peek (evaluator_mutation_boundary.cpp) is
       non-consuming, compares live is_macro_introduced against the
       checkpointed marker column, returns NULL_NODE when clean, and
       cites #3858.
  AC3  test_hygiene_mutate_closed_loop.cpp drives ac3858_1..ac3858_5
       (defined AND dispatched from main).
  AC4  the #3650 rollback face stays intact (rollback prim still gates
       via subtree_has_macro_introduced + deny helper).
  AC5  build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it; no strays
       (no tests/**/test_issue_3858.cpp, no docs/design/3858-*).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRIM = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
MB = ROOT / "src" / "compiler" / "evaluator_mutation_boundary.cpp"
TST = ROOT / "tests" / "compiler" / "test_hygiene_mutate_closed_loop.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def main() -> int:
    prim = PRIM.read_text() if PRIM.exists() else ""
    mb = MB.read_text() if MB.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1: prim gate ordering inside the restore-hygiene-checkpoint lambda.
    prim_idx = prim.find('"mutate:restore-hygiene-checkpoint"')
    span = prim[prim_idx : prim_idx + 2600] if prim_idx != -1 else ""
    peek = span.find("restore_would_demote_macro_introduced(handle)")
    deny = span.find("deny_macro_opt_out_without_mse(ev, demote_id, mev)")
    restore = span.find("restore_hygiene_checkpoint_handle(handle)")
    good = (
        "Issue #3858" in span
        and prim_idx != -1
        and peek != -1
        and deny != -1
        and restore != -1
        and peek < deny < restore
        and "ok = false;" in span
        and "ev.effect_sandbox_mode() != 0" in span
    )
    report("AC1", good, "prim peeks demote → denies via #3650 helper → before restore")

    # AC2: non-consuming peek comparing live MI vs checkpointed markers.
    fn_idx = mb.find("Evaluator::restore_would_demote_macro_introduced(")
    fn_end = mb.find("\n}\n", fn_idx) if fn_idx != -1 else -1
    span = mb[fn_idx:fn_end] if fn_idx != -1 and fn_end != -1 else ""
    good = (
        "Issue #3858" in mb
        and fn_idx != -1
        and "saved[id] != aura::ast::SyntaxMarker::MacroIntroduced" in span
        and "is_macro_introduced(id)" in span
        and "aura::ast::NULL_NODE" in span
        and ".reset()" not in span
        and "return restore_hygiene_checkpoint(cp);" not in span
    )
    report("AC2", good, "peek non-consuming; compares live MI vs checkpointed markers")

    # AC3: the hygiene closed-loop suite drives the #3858 ACs.
    good = all(
        tst.count(fn) >= 2
        for fn in (
            "ac3858_1_restore_demote_denied_without_mse",
            "ac3858_2_restore_with_mse_demotes",
            "ac3858_3_soft_restore_unchanged",
            "ac3858_4_no_demote_restore_allowed",
            "ac3858_5_source_and_no_artifacts",
        )
    )
    report("AC3", good, "test defines and dispatches ac3858_1..5")

    # AC4: the #3650 rollback face stays intact.
    good = (
        "Issue #3650" in prim
        and "subtree_has_macro_introduced(flat, root)" in prim
        and "deny_macro_opt_out_without_mse(ev, root, mev)" in prim
    )
    report("AC4", good, "#3650 rollback MSE gate untouched")

    # AC5: gate wiring — build.py runs the linter and the allowlist lists
    # it; no stray artifact shapes.
    no_stray = (
        not (ROOT / "tests" / "compiler" / "test_issue_3858.cpp").exists()
        and not (ROOT / "docs" / "design" / "3858-0.md").exists()
    )
    wired = "check_restore_checkpoint_mse_3858.py" in build and ("check_restore_checkpoint_mse_3858.py" in allow)
    report("AC5", wired and no_stray, "build.py + allowlist wired; no strays")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
