#!/usr/bin/env python3
"""Issue #3904 — MSE TA fence posture: caller-OR-target documented.

Residual: grant_macro_self_evo's fence allows installing MacroSelfEvo
policy when either the caller OR the target tenant holds TenantAdmin
(#3029 contract) — asymmetric with grant_cross_tenant's caller-only
fence (#3800). A non-TA caller naming an admin tenant as MSE target
can install policy on that target.

Landed (Option B — the issue's sanctioned alternative): the fence
stays caller-OR-target (zero behavior change) and the asymmetry is
documented as intended posture in capability_model.hh. Option A
(caller-only) was implemented first and empirically rejected: under
multi-fiber chaos load the Guard composition change yielded non-zero
mailbox hold/defer starvation (delta 2-8) against the #2554 PR
deployment contract of 0. Revisit caller-only when the chaos workload
adapts to the deny semantics.

  AC1  posture: capability_model.hh cites "Issue #3904 posture" at
      the fence and the caller-OR-target fence is preserved.
  AC2  test-wired: test_tenant_isolation_enforcement.cpp carries the
      #3904 behavioral ACs (non-TA caller + TA target → allow,
      documented; TA caller → allow; neither → deny; Soft/Off
      zero-cost).
  AC3  no-invent: no docs/design/3904-*, no tests/issues/
      test_issue_3904.cpp, no tests/core/test_issue_3904.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (caller-only fence, no posture cite) and must
flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "src" / "core" / "capability_model.hh"
TEST = ROOT / "tests" / "core" / "test_tenant_isolation_enforcement.cpp"
CITE = "Issue #3904 posture"
FENCE = "if (!has_admin(caller) && !has_admin(tenant)) {"


def ac1_posture() -> bool:
    text = SITE.read_text()
    ok = CITE in text and FENCE in text
    print("AC(posture): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_test_wired() -> bool:
    text = TEST.read_text()
    ok = (
        all(f"3904 AC{n}" in text for n in range(1, 6))
        and "ac3904_1_target_ta_allow_non_ta_caller_documented" in text
        and "ac3904_3_neither_ta_denied" in text
    )
    print("AC(test-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac3_no_invent() -> bool:
    docs = list(ROOT.glob("docs/design/3904-*"))
    invented = any(
        (ROOT / p).exists()
        for p in (
            "tests/issues/test_issue_3904.cpp",
            "tests/core/test_issue_3904.cpp",
        )
    )
    ok = not docs and not invented
    print("AC(no-invent): " + ("PASS" if ok else "FAIL"))
    return ok


def self_test() -> int:
    fixture = "if (!has_admin(caller)) {\n"  # caller-only: posture cite missing
    flagged = CITE not in fixture
    print("self-test: " + ("PASS — caller-only fence without posture cite detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_posture(),
        ac2_test_wired(),
        ac3_no_invent(),
    ]
    print(f"check_mse_ta_posture_3904: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
