#!/usr/bin/env python3
"""Issue #3281 linter — mid-bound abort authority for abort × densify/steal.

Production residual (type×linear×TypedMutation review @ c11e8039): abort
restore + concurrent densify/steal rehydrate rely on ordered multi-site
clears, but the #3193/#3232 abort-authority face is process-wide (in_flight
count) — a densify/steal rehydrate for the SAME mid that had an
abort-restore can freeze a green TypeLinearCommitProof / leave residual
CoercionMap / Occurrence entries when the abort restore completed but the
rehydrate interleave is still mid-flight. This adds the MID key.

Gate rows:
  G1  typed_mutation_audit.h cites Issue #3281 and has the 8-slot
      mid-bound abort authority table (kMidAbortAuthoritySlots).
  G2  begin_mid_abort_authority captures a version at abort enter;
      mid_abort_authority_outstanding/version query the slot; Soft/Off
      no-ops (zero-cost).
  G3  all abort sites (evaluator_mutation_boundary.cpp) call
      begin_mid_abort_authority before the ordered clears and
      end_mid_abort_authority after, with a post-clear drift check that
      stamps REJECT proof (no green stamp on torn state).
  G4  rehydrate chokepoint (ConstraintSystem::rehydrate_occurrence_from_persist)
      refuses to freeze green when the mid has an outstanding abort-restore.
  G5  outermost-success persist (aura_outermost_success_persist_occurrence)
      refuses green stamp while the mid has an outstanding abort-restore.
  G6  mismatch counter observable (g_mid_abort_authority_mismatch_total).
  G7  test ACs in tests/compiler/test_coercion_map_abort_rewind.cpp
      (#3102 suite home, #81967).
  G8  build.py wires this linter.
  G9  no docs/design/3281-* (per #1655), no tests/issue*/test_issue_3281.cpp
      (per #81967).

Exit 0 = all rows satisfied.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3281 mid-bound abort authority linter ===")
    typed = read("src/compiler/typed_mutation_audit.h")
    boundary = read("src/compiler/evaluator_mutation_boundary.cpp")
    tci = read("src/compiler/type_checker_impl.cpp")
    test = read("tests/compiler/test_coercion_map_abort_rewind.cpp")
    build = read("build.py")

    must("#3281" in typed, "G1: typed_mutation_audit.h cites Issue #3281")
    must(
        "kMidAbortAuthoritySlots" in typed and "g_mid_abort_authority" in typed,
        "G1: 8-slot mid-bound abort authority table",
    )
    must(
        "begin_mid_abort_authority" in typed
        and "mid_abort_authority_outstanding" in typed
        and "mid_abort_authority_version" in typed
        and "end_mid_abort_authority" in typed,
        "G2: begin/version/outstanding/end helpers present",
    )
    must(
        "mid_abort_authority_hard" in typed
        and "production_defaults_active()" in typed
        and "AuditStrategy::Full" in typed,
        "G2: Soft/Off zero-cost gate (hard = production || Full)",
    )
    # 3 abort sites: begin at ~704/1507/1622, end + post-clear reject at each.
    begin_count = boundary.count("begin_mid_abort_authority")
    end_count = boundary.count("end_mid_abort_authority")
    must(begin_count >= 3, f"G3a: begin_mid_abort_authority at abort sites ({begin_count})")
    must(end_count >= 3, f"G3b: end_mid_abort_authority at abort sites ({end_count})")
    must(
        boundary.count("kTypeLinearProofOutcomeReject") >= 3,
        "G3c: post-clear drift check stamps REJECT proof at each site",
    )
    must(
        "Issue #3281" in boundary and "mid_abort_authority_outstanding" in boundary,
        "G3: boundary cites #3281 + wires mid-bound checks",
    )
    must(
        "mid_abort_authority_outstanding" in tci,
        "G4: rehydrate chokepoint refuses on outstanding mid (AC1)",
    )
    must(
        "aura_outermost_success_persist_occurrence" in boundary and "mid_abort_authority_outstanding" in boundary,
        "G5: outermost-success persist refuses green on outstanding mid (AC2)",
    )
    must(
        "g_mid_abort_authority_mismatch_total" in typed,
        "G6: mismatch counter observable (AC5)",
    )
    must("test_ac3281_mid_bound_abort_authority" in test, "G7: test AC present")
    must(
        "check_mid_bound_abort_authority_3281.py" in build,
        "G8: build.py wires linter",
    )
    must(
        not any(p.name.startswith("3281-") for p in (ROOT / "docs/design").glob("3281-*"))
        if (ROOT / "docs/design").exists()
        else True,
        "G9a: no docs/design/3281-* per #1655",
    )
    must(
        not (ROOT / "tests/issues" / "test_issue_3281.cpp").exists()
        and not (ROOT / "tests/compiler" / "test_issue_3281.cpp").exists(),
        "G9b: no tests/issue*/test_issue_3281.cpp per #81967",
    )

    if failures:
        print(f"\n#3281 linter FAILED: {len(failures)} gate(s)")
        return 1
    print("\nOK #3281 mid_bound_abort_authority: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
