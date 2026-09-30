#!/usr/bin/env python3
"""Issue #4171: blame_ok vacuous on empty frames after mutate under
Production (P0, typed-mutation / provenance).

Hole: aura_typed_audit_fill_from_live_tc treated ANY empty blame chain
as OK — `blame_ok = bc.is_complete() || bc.frames.empty()`. After a real
mutate (evaluator->txn_dirty()) with CS work (commit_cs_has_work()),
Production blame_hard could still allow commit when blame frames were
never recorded: a silent blame/provenance gap for Agent query:type /
audit join (commit reads green, forensics chain empty).

Fix shape (existing SSOTs only — the fill feeds the existing #2221
commit_readiness step-4 blame gate; no second blame model, no new
reason codes):

  out->cs_has_work = tc->commit_cs_has_work();      // filled FIRST
  out->blame_ok = bc.is_complete() || bc.frames.empty();  // vacuous face
  // (#4171 hotfix withdrawal: the mutated-gated deny keyed on txn_dirty /
  // cs_has_work latches poisoned the eval-serving license —
  // TypeLinearCommitProof + #3224/#3130 gates served stale pre-mutation
  // closures process-wide, freezing test_shape_soa_storm_batch #3583 AC1;
  // first bad 96fc3f0c3, bisected; probe blame_ok=true → 14/14 green.
  // Re-land the deny scoped to the commit-audit consumer only.)

Contract (one row per AC):
  AC1  vacuous face restored in evaluator_mutation_boundary.cpp:
       `out->blame_ok = bc.is_complete() || bc.frames.empty();`
       (#4171 hotfix withdrawal — the mutated-gated deny froze mutation
       tracking via the eval-serving license; test_shape_soa_storm_batch
       #3583 AC1 eval freeze. Re-land the deny scoped to the commit-audit
       consumer only.)
       before `out->blame_ok = bc.is_complete() || bc.frames.empty()` the
       old unconditional vacuous expression must be the only arm
  AC2  ordering pin: the cs_has_work fill precedes the blame_ok line in
       the fill body (the mutated face reads the freshly computed value)
  AC3  gate unchanged: commit_readiness step 4 keeps the #2221 blame
       arms — hard reject set("blame", false, 1500) under blame_hard and
       Soft observe set("blame", true, 5000); no new reason codes
  AC4  runtime door: test_occurrence_provenance_chain_completeness.cpp
       hosts the #4171 ACs (ac8_blame_ok_vacuous_mutated: ac4171 issue
       stamp, AC1 blame_hard reject, AC2 Soft observe allow, AC3
       complete frames allow, AC4 vacuous not-mutated preserved) driving
       aura_typed_audit_fill_from_live_tc + commit_readiness; test hooks
       exist (ConstraintSystem + TypeChecker force_last_blame_empty_for_test,
       Evaluator::inject_commit_cs_empty_blame_for_test); no
       tests/**/test_issue_4171.cpp; no docs/design/4171-*
  AC5  build.py wiring + scripts/coverage/root_check_allowlist.txt entry

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

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    boundary = _read("src/compiler/evaluator_mutation_boundary.cpp")
    header = _read("src/compiler/typed_mutation_audit.h")
    tixx = _read("src/compiler/type_checker.ixx")
    eixx = _read("src/compiler/evaluator.ixx")
    etcpp = _read("src/compiler/evaluator_typecheck.cpp")
    test = _read("tests/compiler/test_occurrence_provenance_chain_completeness.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: vacuous face restored (#4171 hotfix withdrawal — eval freeze) ──
    must("out->blame_ok = bc.is_complete() || bc.frames.empty();", "AC1 vacuous face restored", boundary)
    absent("!mutated && bc.frames.empty()", "AC1 #4171 deny arm withdrawn", boundary)
    absent("mutation_boundary_depth() > 0;", "AC1 depth-keyed arm withdrawn", boundary)
    must("#4171", "AC1 fix comment cites issue", boundary)

    # ── AC2: cs_has_work filled before blame_ok inside the fill body ──
    fill_pos = boundary.find("aura_typed_audit_fill_from_live_tc(\n    void* ev")
    if fill_pos == -1:
        fill_pos = boundary.find("void aura_typed_audit_fill_from_live_tc(")
    cs_pos = boundary.find("out->cs_has_work = tc->commit_cs_has_work();", fill_pos)
    blame_pos = boundary.find("out->blame_ok = bc.is_complete() || bc.frames.empty();", fill_pos)
    if fill_pos < 0 or cs_pos < 0 or blame_pos < 0 or cs_pos > blame_pos:
        fails.append("AC2: cs_has_work fill must precede the gated blame_ok line in aura_typed_audit_fill_from_live_tc")

    # ── AC3: #2221 step-4 blame arms unchanged (no new codes) ──
    must('return (set("blame", false, 1500), r);', "AC3 blame hard reject arm", header)
    must('return (set("blame", true, 5000), r);', "AC3 blame Soft observe arm", header)
    must("blame_hard", "AC3 blame_hard flag retained", header)

    # ── AC4: runtime door + test hooks, no invent ──
    must("ac4171: issue stamp", "AC4 test issue stamp", test)
    must("4171 AC1: commit allowed (deny withdrawn — eval freeze)", "AC4 AC1 runtime", test)
    must("4171 AC2: Soft observe keeps commit allowed", "AC4 AC2 runtime", test)
    must("4171 AC3: no false positive on complete chain", "AC4 AC3 runtime", test)
    must("4171 AC4: vacuous empty→ok preserved when not mutated", "AC4 AC4 runtime", test)
    must("aura_typed_audit_fill_from_live_tc(&cs.evaluator(), &in)", "AC4 fill driven", test)
    must("in.blame_hard = true;", "AC4 production face set", test)
    must("force_last_blame_empty_for_test", "AC4 CS hook", tixx)
    must("solve_delta_cs_.force_last_blame_empty_for_test();", "AC4 TypeChecker forward", tixx)
    must("inject_commit_cs_empty_blame_for_test(bool with_cs_work = true) noexcept;", "AC4 evaluator decl", eixx)
    must(
        "void Evaluator::inject_commit_cs_empty_blame_for_test(bool with_cs_work) noexcept {",
        "AC4 evaluator impl",
        etcpp,
    )
    must("ac8_blame_ok_vacuous_mutated();", "AC4 runner dispatched from main", test)
    invent = _read("tests/compiler/test_issue_4171.cpp")
    if invent:
        fails.append("AC4: tests/compiler/test_issue_4171.cpp must not exist (extend the existing post-mutate suite)")
    absent("docs/design/4171", "AC4 no docs/design", boundary + test)

    # ── AC5: wiring ──
    must("check_blame_ok_vacuous_4171", "AC5 build.py wiring", build)
    must("check_blame_ok_vacuous_4171.py", "AC5 allowlist entry", allow)

    if fails:
        print("FAIL #4171 blame_ok_vacuous:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #4171 blame_ok_vacuous: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
