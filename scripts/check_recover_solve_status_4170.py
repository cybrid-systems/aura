#!/usr/bin/env python3
# scripts/check_recover_solve_status_4170.py -- Issue #4170 source-cite gate.
#
# AC1: TypeChecker::try_occurrence_hard_face_full_solve_recover stamps
#      last_delta_solve_status_ = SOLVED on the live commit TC when its full
#      solve reaches SOLVED (the recover outcome becomes the live truth).
#      Without the stamp, commit_readiness's #3108 re-gate false-rejected a
#      legitimate recover off the stale pre-recover snapshot.
# AC2: typed_mutation_audit.h declares the post-recover re-sample seam —
#      aura_typed_audit_recover_live_solve_status C ABI + the
#      kAuraTypedAuditSolveStatusUnknown (0xFF) sentinel — and wires the
#      re-sample in ahead of every #3108 re-gate (step 2 / step 6 / 6c).
# AC3: evaluator_mutation_boundary.cpp implements the stateless validity
#      gate: test override installed -> sentinel; no live commit TC ->
#      sentinel; else the live TC's last_delta_solve_status. Plus the
#      test-only seed ABI for the live-path ACs.
# AC4: #3623/#3108 composition preserved — the re-gate predicate
#      `recovered && in.solve_status != 0` still appears at all three sites
#      and both #3108 cite comments survive (no pinned-fixture flip).
# AC5: runtime ACs live in tests/compiler/test_partial_cone_commit_gate.cpp
#      (ac4170_*); light-link stubs in src/compiler/test_concurrent_stubs.cpp;
#      no test_issue_4170.cpp, no docs/design/*4170*; build.py + allowlist
#      wiring present.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

IXX = "src/compiler/type_checker.ixx"
H = "src/compiler/typed_mutation_audit.h"
CPP = "src/compiler/evaluator_mutation_boundary.cpp"
STUBS = "src/compiler/test_concurrent_stubs.cpp"
TEST = "tests/compiler/test_partial_cone_commit_gate.cpp"
BUILD = "build.py"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_recover_solve_status_4170"
STAMP = "last_delta_solve_status_ = SolveResult::SOLVED;"
RESAMPLE = "aura_typed_audit_recover_live_solve_status()"
SENTINEL_CHECK = "live_status != kAuraTypedAuditSolveStatusUnknown"
REGATE = "recovered && in.solve_status != 0"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(ixx: str, h: str, cpp: str, stubs: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — recover stamps SOLVED on the live TC.
    must("Issue #4170", "AC1 recover cite", ixx)
    rec = ixx.find("bool try_occurrence_hard_face_full_solve_recover() noexcept")
    rec_end = ixx.find("Issue #3380: recover is invoked", rec if rec >= 0 else 0)
    if rec == -1 or rec_end == -1 or rec_end < rec:
        fails.append("AC1: recover function window not found")
    else:
        win = ixx[rec:rec_end]
        must(STAMP, "AC1 SOLVED stamp inside recover", win)

    # AC2 — header seam + re-sample ahead of every #3108 re-gate.
    must("kAuraTypedAuditSolveStatusUnknown = 0xFF", "AC2 sentinel constant", h)
    must('extern "C" std::uint8_t ' + RESAMPLE, "AC2 re-sample ABI declared", h)
    must(
        'extern "C" void aura_typed_audit_test_seed_commit_solve_status(int status) noexcept',
        "AC2 seed ABI declared",
        h,
    )
    must("Issue #4170", "AC2 re-sample cite", h)
    n_resample = h.count(RESAMPLE)
    if n_resample < 4:  # 1 decl (with parens) + 3 call sites
        fails.append(f"AC2: expected >=4 re-sample occurrences (decl + 3 sites), found {n_resample}")
    n_sentinel = h.count(SENTINEL_CHECK)
    if n_sentinel < 3:
        fails.append(f"AC2: expected >=3 sentinel checks (one per site), found {n_sentinel}")

    # AC3 — boundary TU implements the stateless gate + seed ABI.
    must("aura_typed_audit_recover_live_solve_status", "AC3 re-sample implemented", cpp)
    must("g_tls_test_recover_fn != nullptr", "AC3 override -> sentinel gate", cpp)
    must("aura_typed_audit_test_seed_commit_solve_status", "AC3 seed ABI implemented", cpp)
    must("note_last_delta_solve_status_for_test", "AC3 seed routes to TC setter", cpp)

    # AC4 — #3623/#3108 composition preserved (no pinned-fixture flip).
    n_regate = h.count(REGATE)
    if n_regate < 3:
        fails.append(f"AC4: expected >=3 re-gate sites preserved, found {n_regate}")
    must("Issue #3108: re-gate recover on solve_status==SOLVED (0)", "AC4 cone cite", h)
    must("Issue #3108: second re-gate", "AC4 cone/empty cite", h)
    must("Issue #3623: #3108 SOLVED re-gate", "AC4 6c cite", h)

    # AC5 — runtime ACs + light-link stubs + wiring; forbidden artifacts.
    for fn in (
        "ac4170_1_live_recover_solved_allows",
        "ac4170_2_override_true_conflict_still_fails_closed",
        "ac4170_3_source_and_seams",
    ):
        must(fn, "AC5 test AC defined", test)
        must(fn + "();", "AC5 test AC invoked", test)
    must("aura_typed_audit_recover_live_solve_status", "AC5 light-link stub", stubs)
    must(LINTER, "wiring build.py", build)
    must(LINTER + ".py", "wiring allowlist entry", allow)
    if (ROOT / "tests" / "compiler" / "test_issue_4170.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_4170.cpp (#81934)")
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir():
        for p in design_dir.glob("*"):
            if "4170" in p.name:
                fails.append(f"AC5: forbidden docs/design/{p.name} (#1655)")
                break

    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_ixx = (
            "bool try_occurrence_hard_face_full_solve_recover() noexcept\n"
            "Issue #4170\n" + STAMP + "\nIssue #3380: recover is invoked\n"
        )
        sample_h = (
            "kAuraTypedAuditSolveStatusUnknown = 0xFF\n"
            'extern "C" std::uint8_t ' + RESAMPLE + "\n"
            'extern "C" void aura_typed_audit_test_seed_commit_solve_status(int status) noexcept\n'
            "Issue #4170\n"
            + RESAMPLE
            + "\n"
            + SENTINEL_CHECK
            + "\n"
            + REGATE
            + "\n"
            + RESAMPLE
            + "\n"
            + SENTINEL_CHECK
            + "\n"
            + REGATE
            + "\n"
            + RESAMPLE
            + "\n"
            + SENTINEL_CHECK
            + "\n"
            + REGATE
            + "\n"
            + "Issue #3108: re-gate recover on solve_status==SOLVED (0)\n"
            + "Issue #3108: second re-gate\n"
            + "Issue #3623: #3108 SOLVED re-gate\n"
        )
        sample_cpp = (
            "aura_typed_audit_recover_live_solve_status\n"
            "g_tls_test_recover_fn != nullptr\n"
            "aura_typed_audit_test_seed_commit_solve_status\n"
            "note_last_delta_solve_status_for_test\n"
        )
        sample_stubs = "aura_typed_audit_recover_live_solve_status\n"
        sample_test = "".join(
            fn + "\n" + fn + "();\n"
            for fn in (
                "ac4170_1_live_recover_solved_allows",
                "ac4170_2_override_true_conflict_still_fails_closed",
                "ac4170_3_source_and_seams",
            )
        )
        ok_fails = _rows(sample_ixx, sample_h, sample_cpp, sample_stubs, sample_test, LINTER, LINTER + ".py")
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        neg_fails = _rows("", "", "", "", "", "", "")
        if len(neg_fails) < 12:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(IXX), _read(H), _read(CPP), _read(STUBS), _read(TEST), _read(BUILD), _read(ALLOW))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("recover solve-status re-sample (#4170) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
