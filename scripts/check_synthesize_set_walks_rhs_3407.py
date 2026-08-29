#!/usr/bin/env python3
"""Issue #3407: synthesize_flat NodeTag::Set must walk RHS + unify with var type + report ground mismatch.

Residual from #3044 / #976: NodeTag::Set was added to the bidirectional
covered-tag table so it does NOT hit `note_uncovered_bidirectional_tag`
(#3330). The residual is the opposite shape — Set is covered but the
synthesize_flat arm is a no-op (returns Void without walking RHS).

check_flat Set already walks RHS + unifies + reports ground mismatch
(the intended contract, ~line 7558 of type_checker_impl.cpp). synthesize_flat
Set was the odd arm: under Production+Strict, (set! x "hi") where x : Int
never fired the #3202 ground reject, query:type / next mutate saw green
Void on the Set node, env_ was not rebound, and occurrence goals keyed on
x were not dropped. I1 «渐进不撒谎» + I4 «过期窄化仍能用» on the assignment
face (not the uncovered-tag face).

Contract:
  AC1 The `case Tag::Set:` arm inside `synthesize_flat` (in
     src/compiler/type_checker_impl.cpp) calls synthesize_flat on the
     RHS child, looks up the var in env_, unifies val_type vs var_type
     via consistent_unify, and reports ground mismatch via
     maybe_report_ground_inconsistency.
  AC2 synthesize_flat_begin / infer_flat / infer_flat_partial walk the
     Set RHS via synthesize_flat (Begin children go through synthesize_flat).
  AC3 check_flat Set contract unchanged (same unify + ground report,
     plus expected unification).
  AC4 No `docs/design/3407-*` (per #1655); no `test_issue_3407.cpp`
     (per #81934). Test file is test_synthesize_set_walks_rhs.cpp.
  AC5 Source-cite #3407 + build.py registration; no design docs.

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


def _count(hay: str, needle: str) -> int:
    c = 0
    pos = 0
    while True:
        pos = hay.find(needle, pos)
        if pos == -1:
            return c
        c += 1
        pos += len(needle)


def main() -> int:
    fails: list[str] = []

    tci = _read("src/compiler/type_checker_impl.cpp")
    build = _read("build.py")

    # AC1: synthesize_flat Set case walks RHS + unifies + reports ground.
    synth_pos = tci.find("TypeId InferenceEngine::synthesize_flat(")
    if synth_pos == -1:
        fails.append(
            "AC1: synthesize_flat helper not found in "
            "src/compiler/type_checker_impl.cpp (#3407 contract anchor missing)"
        )
        synth_after = ""
    else:
        synth_after = tci[synth_pos:]

    set_pos = synth_after.find("case Tag::Set:")
    if set_pos == -1:
        fails.append(
            "AC1: synthesize_flat Set case not found — #3407 fix not shipped (synthesize_flat Set was the odd arm)"
        )
    else:
        break_pos = synth_after.find("break;", set_pos)
        if break_pos == -1:
            fails.append("AC1: synthesize_flat Set case has no `break;` (case shape changed unexpectedly)")
        else:
            set_body = synth_after[set_pos:break_pos]
            if "synthesize_flat(flat, pool, val_id" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case does NOT call "
                    "synthesize_flat on the RHS child (val_id) — "
                    "RHS still not walked under synthesize"
                )
            if "env_.lookup" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case does NOT look up "
                    "var_name in env_ — assignment face stale narrowing "
                    "after set!"
                )
            if "consistent_unify(val_type, var_type)" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case does NOT unify "
                    "val_type with var_type — Production+Strict ground "
                    'mismatch (e.g. (set! x "hi") where x : Int) '
                    "would not fire #3202 ground reject"
                )
            if "maybe_report_ground_inconsistency(val_type, var_type)" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case does NOT call "
                    "maybe_report_ground_inconsistency on val_type vs "
                    "var_type — ground mismatch diagnostic dropped"
                )
            if "#3407" not in set_body:
                fails.append(
                    "AC1: synthesize_flat Set case is missing the #3407 "
                    "source-cite anchor (comment must reference #3407)"
                )

    # AC2: synthesize_flat_begin / infer_flat / infer_flat_partial walk Set.
    # Begin delegates to synthesize_flat for every child. infer_flat
    # defaults to synthesize_flat for the root. infer_flat_partial calls
    # synthesize_flat on every node in the partial cone.
    if "synthesize_flat_begin" not in tci:
        fails.append("AC2: synthesize_flat_begin not found")
    if "TypeId InferenceEngine::infer_flat(" not in tci:
        fails.append("AC2: infer_flat not found")
    if "infer_flat_partial" not in tci:
        fails.append("AC2: infer_flat_partial not found")

    # AC3: check_flat Set contract unchanged (same unify + ground report,
    # plus expected unification).
    check_pos = tci.find("InferenceEngine::check_flat(")
    if check_pos == -1:
        fails.append("AC3: check_flat helper not found (regressed)")
    else:
        check_after = tci[check_pos:]
        set_branch_pos = check_after.find("NodeTag::Set")
        if set_branch_pos == -1:
            fails.append("AC3: check_flat Set branch not found — check_flat contract regressed")
        else:
            set_branch = check_after[set_branch_pos:]
            if "synthesize_flat(flat, pool, val_id" not in set_branch:
                fails.append("AC3: check_flat Set no longer synthesizes RHS (contract regressed)")
            if "env_.lookup" not in set_branch:
                fails.append("AC3: check_flat Set no longer looks up var_name (contract regressed)")
            if "consistent_unify(val_type, var_type)" not in set_branch:
                fails.append("AC3: check_flat Set no longer unifies val_type with var_type (contract regressed)")
            if "maybe_report_ground_inconsistency(val_type, var_type)" not in set_branch:
                fails.append("AC3: check_flat Set no longer reports ground mismatch (contract regressed)")
            if "consistent_unify(val_type, expected)" not in set_branch:
                fails.append(
                    "AC3: check_flat Set no longer unifies with expected "
                    "(contract regressed — check_flat must still unify "
                    "with the expected context type)"
                )

    # AC4: no docs/design/3407-*.md, no test_issue_3407.cpp.
    if list((ROOT / "docs" / "design").glob("3407-*.md")):
        fails.append("AC4: docs/design/3407-*.md exists — design docs banned per #1655")
    if (ROOT / "tests" / "compiler" / "test_issue_3407.cpp").is_file():
        fails.append(
            "AC4: tests/compiler/test_issue_3407.cpp exists — must extend "
            "existing test or use source-cite test (test_synthesize_set_walks_rhs.cpp) "
            "per #81934"
        )
    if not (ROOT / "tests" / "compiler" / "test_synthesize_set_walks_rhs.cpp").is_file():
        fails.append("AC4: tests/compiler/test_synthesize_set_walks_rhs.cpp missing (source-cite test not created)")

    # AC5: build.py registration.
    if "check_synthesize_set_walks_rhs_3407" not in build:
        fails.append(
            "AC5: build.py does not register "
            "check_synthesize_set_walks_rhs_3407 (linter not wired into "
            "the coverage gate)"
        )

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3407 synthesize_flat Set walks RHS contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
