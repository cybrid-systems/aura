#!/usr/bin/env python3
"""Issue #3904 — MSE TA fence posture (SUPERSEDED by #4133).

grant_macro_self_evo's fence originally allowed installing MacroSelfEvo
policy when either the caller OR the target tenant held TenantAdmin
(#3029 contract), asymmetric with grant_cross_tenant's caller-only fence
(#3800). Issue #4133 flipped the fence to CALLER-ONLY, closing the
non-TA-caller → TA-target registry mint face. This linter now pins the
SUPERSEDING contract: capability_model.hh cites Issue #4133 at the fence
and the caller-only fence is present (the caller-OR-target line is gone).

  AC1  supersession: capability_model.hh cites "Issue #4133" and the
      caller-only fence (`if (!has_admin(caller)) {`) is present.
  AC2  test-wired: test_tenant_isolation_enforcement.cpp carries the
      flipped #4133 behavioral AC (non-TA caller + TA target → deny) and
      the unchanged #3904-era arms (TA caller → allow; neither → deny).
  AC3  no-invent: no docs/design/3904-*, no tests/issues/
      test_issue_3904.cpp, no tests/core/test_issue_3904.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (caller-OR-target fence, no #4133 cite) and must
flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "src" / "core" / "capability_model.hh"
TEST = ROOT / "tests" / "core" / "test_tenant_isolation_enforcement.cpp"
CITE = "Issue #4133"
FENCE = "if (!has_admin(caller)) {"
OLD_FENCE = "has_admin(caller) && !has_admin(tenant)"


def ac1_superseded_posture() -> bool:
    text = SITE.read_text()
    ok = CITE in text and FENCE in text and OLD_FENCE not in text
    print("AC(superseded-posture): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_test_wired() -> bool:
    text = TEST.read_text()
    ok = (
        "ac4133_1_target_ta_non_ta_caller_denied" in text
        and "ac3904_2_caller_with_ta_allow_unchanged" in text
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
    fixture = "if (!has_admin(caller) && !has_admin(tenant)) {\n"  # stale dual fence
    flagged = OLD_FENCE in fixture and CITE not in fixture
    print("self-test: " + ("PASS — caller-OR-target fence without #4133 cite detected" if flagged else "FAIL"))
    return 0 if flagged else 1


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    results = [
        ac1_superseded_posture(),
        ac2_test_wired(),
        ac3_no_invent(),
    ]
    print(f"check_mse_ta_posture_3904: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
