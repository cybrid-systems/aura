#!/usr/bin/env python3
"""Issue #3408: Set assignment hygiene — drop stale OccurrenceGoal / invalidate predicate memo / mark touched on delta.

Sibling of #3407 (Set synthesize walks RHS). Even after RHS is walked
+ unified, assignment still does not touch the occurrence table or
TypeEnv binding. The Set node carries the assigned name in v.sym_id
but NodeTag::Set is absent from `infer_flat_partial` affected_names
tag list, so `invalidate_predicate_memo_for_var_names` + goal drop
never see the assigned identifier unless some other node in the cone
happens to be a Variable use of the same name.

Reuse existing counters (`occurrence_goal_stale_drop_total`,
`predicate_memo_selective_invalidate_total`). No new query key, no
new OccurrenceGoal kind, no env_.bind over a concrete non-Dyn binding.

Contract:
  AC1 synthesize_flat Set case (after #3407) calls
     invalidate_predicate_memo_for_var_names({var_name}) +
     cs_.drop_occurrence_goals_for_var_type(var_type) +
     cs_.mark_touched_on_delta(var_type, /*occurrence_narrow=*/false).
  AC2 check_flat Set case (after #3407) calls the same three hygiene
     calls + still unifies with expected (check_flat contract unchanged).
  AC3 `infer_flat_partial` affected_names tag list includes NodeTag::Set
     alongside Variable / Define / Let / LetRec / Lambda.
  AC4 Production+Strict mismatch still fails via #3407 (this ticket does
     not weaken that reject — reuse consistent_unify + set_node_error).
  AC5 Soft/Off: no new query key; reuse stale-drop / selective-invalidate
     counters. Reuse `ConstraintSystem::drop_occurrence_goals_for_var_type`
     helper (new in this ticket, declared in type_checker.ixx, defined in
     type_checker_impl.cpp).
  AC6 No `docs/design/3408-*` (per #1655); no `test_issue_3408.cpp`
     (per #81934). Extends test_synthesize_set_walks_rhs.cpp.
  AC7 Source-cite #3408 + build.py registration; no design docs.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    tci = _read("src/compiler/type_checker_impl.cpp")
    tcx = _read("src/compiler/type_checker.ixx")
    build = _read("build.py")
    test = _read("tests/compiler/test_synthesize_set_walks_rhs.cpp")

    synth_pos = tci.find("TypeId InferenceEngine::synthesize_flat(")
    if synth_pos == -1:
        fails.append(
            "AC1: synthesize_flat helper not found in "
            "src/compiler/type_checker_impl.cpp (#3408 contract anchor missing)"
        )
        synth_after = ""
    else:
        synth_after = tci[synth_pos:]

    set_pos = synth_after.find("case Tag::Set:")
    if set_pos == -1:
        fails.append("AC1: synthesize_flat Set case not found — #3407 fix not shipped")
    else:
        break_pos = synth_after.find("break;", set_pos)
        if break_pos == -1:
            fails.append("AC1: synthesize_flat Set case has no `break;` (case shape changed unexpectedly)")
        else:
            set_body = synth_after[set_pos:break_pos]
            if "invalidate_predicate_memo_for_var_names({var_name})" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case does NOT call "
                    "invalidate_predicate_memo_for_var_names({var_name}) — "
                    "predicate memo for assigned name not invalidated"
                )
            if "cs_.drop_occurrence_goals_for_var_type(var_type)" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case does NOT call "
                    "cs_.drop_occurrence_goals_for_var_type(var_type) — "
                    "stale OccurrenceGoal not dropped (I4 after set!)"
                )
            if "mark_touched_on_delta(var_type, /*occurrence_narrow=*/false)" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case does NOT call "
                    "mark_touched_on_delta(var_type, /*occurrence_narrow=*/false) "
                    "— delta not marked for non-narrow Set assignment"
                )
            if "#3408" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case is missing the #3408 "
                    "source-cite anchor (comment must reference #3408)"
                )

    check_pos = tci.find("InferenceEngine::check_flat(")
    if check_pos == -1:
        fails.append("AC2: check_flat helper not found (regressed)")
    else:
        check_after = tci[check_pos:]
        set_branch_pos = check_after.find("NodeTag::Set")
        if set_branch_pos == -1:
            fails.append("AC2: check_flat Set branch not found — check_flat contract regressed")
        else:
            set_branch = check_after[set_branch_pos:]
            if "invalidate_predicate_memo_for_var_names({var_name})" not in set_branch:
                fails.append(
                    "AC2: check_flat Set does NOT call "
                    "invalidate_predicate_memo_for_var_names({var_name}) — "
                    "predicate memo for assigned name not invalidated"
                )
            if "cs_.drop_occurrence_goals_for_var_type(var_type)" not in set_branch:
                fails.append(
                    "AC2: check_flat Set does NOT call "
                    "cs_.drop_occurrence_goals_for_var_type(var_type) — "
                    "stale OccurrenceGoal not dropped (I4 after set!)"
                )
            if "mark_touched_on_delta(var_type, /*occurrence_narrow=*/false)" not in set_branch:
                fails.append(
                    "AC2: check_flat Set does NOT call "
                    "mark_touched_on_delta(var_type, /*occurrence_narrow=*/false) "
                    "— delta not marked for non-narrow Set assignment"
                )
            if "consistent_unify(val_type, expected)" not in set_branch:
                fails.append(
                    "AC2: check_flat Set no longer unifies with expected "
                    "(#3407 contract regressed — check_flat must still unify "
                    "with the expected context type)"
                )

    # AC3: NodeTag::Set must be in the infer_flat_partial affected_names
    # tag list. Use a unique anchor: the `affected_names.reserve(affected.size())`
    # line (only infer_flat_partial has this specific reserve + insert combo).
    aff_reserve_pos = tci.find("affected_names.reserve(affected.size())")
    if aff_reserve_pos == -1:
        fails.append(
            "AC3: infer_flat_partial affected_names.reserve(affected.size()) "
            "block not found — shape changed unexpectedly"
        )
    else:
        # Scope the tag list: from the reserve line to the insert line.
        aff_insert_pos = tci.find("affected_names.insert(std::string(nm));", aff_reserve_pos)
        if aff_insert_pos == -1:
            fails.append(
                "AC3: affected_names.insert(std::string(nm)) not found after reserve — shape changed unexpectedly"
            )
        else:
            tag_list = tci[aff_reserve_pos:aff_insert_pos]
            if "NodeTag::Set" not in tag_list:
                fails.append(
                    "AC3: infer_flat_partial affected_names tag list does NOT "
                    "include NodeTag::Set — Set node's v.sym_id is invisible to "
                    "invalidate_predicate_memo_for_var_names unless a Variable "
                    "use of the same name is also in the cone"
                )
            if "NodeTag::Variable" not in tag_list:
                fails.append(
                    "AC3: NodeTag::Variable missing from affected_names tag list "
                    "(regression — Variable was the baseline)"
                )
            if "NodeTag::Define" not in tag_list:
                fails.append(
                    "AC3: NodeTag::Define missing from affected_names tag list (regression — Define was the baseline)"
                )

    if "set_node_error" not in synth_after:
        fails.append(
            "AC4: synthesize_flat Set case does NOT call set_node_error — "
            "#3407 Production+Strict reject path regressed (this ticket "
            "must not weaken #3407)"
        )
    if "consistent_unify(val_type, var_type)" not in synth_after:
        fails.append(
            "AC4: synthesize_flat Set case does NOT call "
            "consistent_unify(val_type, var_type) — #3407 unify regressed "
            "(this ticket must not weaken #3407)"
        )

    if "drop_occurrence_goals_for_var_type" not in tcx:
        fails.append(
            "AC5: type_checker.ixx does NOT declare "
            "drop_occurrence_goals_for_var_type (CS helper not added to "
            "header — linter cannot verify the contract)"
        )
    if "drop_occurrence_goals_for_var_type" not in tci:
        fails.append(
            "AC5: type_checker_impl.cpp does NOT define drop_occurrence_goals_for_var_type (CS helper not implemented)"
        )
    if (
        "occurrence_goal_stale_drop_total.fetch_add(dropped"
        not in tci.split("drop_occurrence_goals_for_var_type", 1)[-1]
    ):
        fails.append(
            "AC5: drop_occurrence_goals_for_var_type does NOT bump "
            "occurrence_goal_stale_drop_total (must reuse existing counter "
            "per issue AC4)"
        )
    if "check_set_assignment_hygiene_3408" not in build:
        fails.append(
            "AC7: build.py does not register "
            "check_set_assignment_hygiene_3408 (linter not wired into "
            "the coverage gate)"
        )

    if list((ROOT / "docs" / "design").glob("3408-*.md")):
        fails.append("AC6: docs/design/3408-*.md exists — design docs banned per #1655")
    if (ROOT / "tests" / "compiler" / "test_issue_3408.cpp").is_file():
        fails.append(
            "AC6: tests/compiler/test_issue_3408.cpp exists — must extend "
            "existing test_synthesize_set_walks_rhs.cpp per #81934"
        )
    if "AC5: #3408 synthesize_flat Set" not in test:
        fails.append(
            "AC6: test_synthesize_set_walks_rhs.cpp missing AC5 for #3408 (synthesize_flat Set assignment hygiene)"
        )
    if "AC6: #3408 check_flat Set" not in test:
        fails.append("AC6: test_synthesize_set_walks_rhs.cpp missing AC6 for #3408 (check_flat Set assignment hygiene)")
    if "AC7: #3408 infer_flat_partial" not in test:
        fails.append(
            "AC6: test_synthesize_set_walks_rhs.cpp missing AC7 for #3408 "
            "(infer_flat_partial affected_names includes Set)"
        )

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3408 Set assignment hygiene contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
