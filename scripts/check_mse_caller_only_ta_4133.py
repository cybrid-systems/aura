#!/usr/bin/env python3
"""Issue #4133 — MSE caller-only TA fence (supersedes #3904 posture).

grant_macro_self_evo's production (Restricted/Strict) fence requires
TenantAdmin on the CALLER principal only — aligned with
try_grant_cross_tenant_privileged's caller-only fence (#3800). This
supersedes #3029/#3904's caller-OR-target posture: under caller-OR-target
a non-TA caller could mint MacroSelfEvo onto a TA-holding target via the
registry API, widening privilege-mint power scoped to the TARGET's admin
bit. Target-only TA is deny; same-tenant self-mint still requires caller
TA. SE reason (macro-self-evo-grant-needs-tenant-admin) and the deny
counter (capability_macro_self_evo_grant_deny_total) stay stable.

  AC1  fence-flip: capability_model.hh cites "Issue #4133", the fence is
      caller-only (`if (!has_admin(caller)) {`), and the dual-track
      caller-OR-target line is gone.
  AC2  stable deny surface: SE reason + deny counter names unchanged, and
      the Soft/Off zero-cost gate still wraps the fence inside
      grant_macro_self_evo.
  AC3  alignment: workspace_isolation.hh still pins the #3800 caller-only
      TA pattern (the SSOT this fence aligns with).
  AC4  test-wired: test_tenant_isolation_enforcement.cpp carries the five
      #4133 ACs (ac4133_1..ac4133_5); test_macro_self_evo_capability.cpp
      carries the direct registry mint checks (#4133 AC1).
  AC5  no-invent: no docs/design/4133-*, no tests/issues/
      test_issue_4133.cpp, no tests/core/test_issue_4133.cpp.

Exit 0 only when all ACs pass. `--self-test` runs the detector over an
inline stale fixture (caller-OR-target fence, no #4133 cite) and must
flag it.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "src" / "core" / "capability_model.hh"
ISO = ROOT / "src" / "core" / "workspace_isolation.hh"
TEST = ROOT / "tests" / "core" / "test_tenant_isolation_enforcement.cpp"
MSE_TEST = ROOT / "tests" / "compiler" / "test_macro_self_evo_capability.cpp"
CITE = "Issue #4133"
FENCE = "if (!has_admin(caller)) {"
OLD_FENCE = "has_admin(caller) && !has_admin(tenant)"
REASON = "macro-self-evo-grant-needs-tenant-admin"
COUNTER = "capability_macro_self_evo_grant_deny_total"


def ac1_fence_flip() -> bool:
    text = SITE.read_text()
    ok = CITE in text and FENCE in text and OLD_FENCE not in text
    print("AC(fence-flip): " + ("PASS" if ok else "FAIL"))
    return ok


def ac2_stable_deny_surface() -> bool:
    text = SITE.read_text()
    ok = REASON in text and COUNTER in text
    if ok:
        # Soft/Off zero-cost: the Off gate must wrap the fence inside the
        # grant_macro_self_evo body (fence sits between the gate and the
        # deny reason emission).
        start = text.find("bool grant_macro_self_evo")
        end = text.find(REASON, start)
        region = text[start:end] if start != -1 and end != -1 else ""
        ok = "if (mode != EffectSandboxMode::Off) {" in region
    print("AC(stable-deny-surface): " + ("PASS" if ok else "FAIL"))
    return ok


def ac3_caller_only_alignment() -> bool:
    text = ISO.read_text()
    ok = "const bool caller_ta = has_effect(caller_eff, Effect::TenantAdmin);" in text
    print("AC(caller-only-alignment): " + ("PASS" if ok else "FAIL"))
    return ok


def ac4_test_wired() -> bool:
    text = TEST.read_text()
    fns = [
        "ac4133_1_target_ta_non_ta_caller_denied",
        "ac4133_2_caller_ta_mint_foreign_target_lands",
        "ac4133_3_soft_off_zero_cost_unchanged",
        "ac4133_4_concurrent_ta_revoke_mint_deny_table_unchanged",
        "ac4133_5_prim_mse_seed_refuses_with_base_grant",
    ]
    ok = all(f in text for f in fns) and all(f"#4133 AC{n}" in text for n in range(1, 6))
    ok = ok and "#4133 AC1" in MSE_TEST.read_text()
    print("AC(test-wired): " + ("PASS" if ok else "FAIL"))
    return ok


def ac5_no_invent() -> bool:
    docs = list(ROOT.glob("docs/design/4133-*"))
    invented = any(
        (ROOT / p).exists()
        for p in (
            "tests/issues/test_issue_4133.cpp",
            "tests/core/test_issue_4133.cpp",
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
        ac1_fence_flip(),
        ac2_stable_deny_surface(),
        ac3_caller_only_alignment(),
        ac4_test_wired(),
        ac5_no_invent(),
    ]
    print(f"check_mse_caller_only_ta_4133: {sum(results)}/{len(results)} ACs pass")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
