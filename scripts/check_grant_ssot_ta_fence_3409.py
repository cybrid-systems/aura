#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #3409: CapabilityRegistry::grant SSOT TenantAdmin fence.
#
# AC1 — CapabilityRegistry::grant has TA guard in grant_locked (caller_principal param).
# AC2 — grant() signature accepts TenantId caller_principal (default 0).
# AC3 — Evaluator path forwards caller_principal = capability_tenant_id_.
# AC4 — Deny reuses capability_macro_self_evo_grant_deny_total counter (no new metrics).
# AC5 — security_defaults.hh kernel render bootstrap grants tenant=0 / Render.
# AC6 — No docs/design/3409-*.md (banned per #1655) and no
#        tests/core/test_issue_3409.cpp (must extend
#        test_tenant_isolation_enforcement.cpp per #81934).
# AC7 — test_tenant_isolation_enforcement.cpp carries AC7/AC8/AC9 markers for #3409.
# AC7 — build.py registers check_grant_ssot_ta_fence_3409 in the coverage gate.
#
# Self-test:
#   python3 scripts/check_grant_ssot_ta_fence_3409.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    fails: list[str] = []

    cap = (ROOT / "src" / "core" / "capability_model.hh").read_text()
    eval_src = (ROOT / "src" / "compiler" / "evaluator_security.cpp").read_text()
    sd = (ROOT / "src" / "compiler" / "security_defaults.hh").read_text()
    test = (ROOT / "tests" / "core" / "test_tenant_isolation_enforcement.cpp").read_text()
    build = (ROOT / "build.py").read_text()

    # AC1 — TA fence lives in grant_locked.
    if "grant-ssot-needs-tenant-admin" not in cap:
        fails.append(
            "AC1: capability_model.hh missing SE reason 'grant-ssot-needs-tenant-admin' (TA fence in grant_locked)"
        )

    # AC2 — grant() takes a TenantId caller_principal parameter.
    if "TenantId caller_principal = 0" not in cap:
        fails.append("AC2: capability_model.hh grant() missing TenantId caller_principal parameter")

    # AC3 — Evaluator path forwards capability_tenant_id_ as caller_principal.
    if "capability_tenant_id_" not in eval_src:
        fails.append("AC3: evaluator_security.cpp missing capability_tenant_id_ thread-local / member")
    if "Evaluator::grant_capability" not in eval_src:
        fails.append("AC3: evaluator_security.cpp missing Evaluator::grant_capability definition")
    if not any(line.lstrip().startswith("void Evaluator::grant_capability") for line in eval_src.splitlines()):
        fails.append("AC3: evaluator_security.cpp does not define Evaluator::grant_capability")
    # Caller passes capability_tenant_id_ as 7th arg to grant().
    if "g_capability_registry().grant(" not in eval_src:
        fails.append("AC3: evaluator_security.cpp does not call g_capability_registry().grant()")
    grant_call_idx = eval_src.rfind("g_capability_registry().grant(")
    tail = eval_src[grant_call_idx : grant_call_idx + 600]
    if "capability_tenant_id_" not in tail:
        fails.append("AC3: Evaluator::grant_capability does not forward capability_tenant_id_ as caller_principal")

    # AC4 — Reuse existing counter (no new metrics field added by #3409).
    if "capability_macro_self_evo_grant_deny_total" not in cap:
        fails.append(
            "AC4: capability_model.hh missing reuse of capability_macro_self_evo_grant_deny_total deny counter"
        )
    # Sanity: no new metrics struct field with "grant_ssot" / "3409".
    if "grant_ssot_ta_fence_deny_total" in cap or "grantssot" in cap.lower():
        fails.append("AC4: capability_model.hh must NOT add a new metrics field (reuse existing counter)")

    # AC5 — security_defaults.hh kernel render bootstrap (tenant=0 / Render).
    if "grant_render_kernel_principal" not in sd:
        fails.append("AC5: security_defaults.hh missing grant_render_kernel_principal() kernel bootstrap")
    if '/*tenant=*/0, "render", Effect::Render' not in sd and '/*tenant=*/0, "render", Effect::Render' not in sd:
        fails.append("AC5: kernel render bootstrap tenant=0 Render missing from security_defaults.hh")

    # AC6 — No docs/design/3409-*.md (banned per #1655); no tests/core/test_issue_3409.cpp.
    if list((ROOT / "docs" / "design").glob("3409-*.md")):
        fails.append("AC6: docs/design/3409-*.md exists — design docs banned per #1655")
    if (ROOT / "tests" / "core" / "test_issue_3409.cpp").is_file():
        fails.append(
            "AC6: tests/core/test_issue_3409.cpp exists — must extend existing test_tenant_isolation_enforcement.cpp per #81934"
        )

    # AC7 — Test markers in test_tenant_isolation_enforcement.cpp.
    if "AC7: #3409 grant() SSOT TA fence" not in test:
        fails.append("AC6: test_tenant_isolation_enforcement.cpp missing AC7 for #3409 (grant_locked TA fence)")
    if "AC8: #3409 Evaluator::grant_capability" not in test:
        fails.append(
            "AC6: test_tenant_isolation_enforcement.cpp missing AC8 for #3409 (Evaluator passes caller_principal)"
        )
    if "AC9: no docs/design/3409-*" not in test:
        fails.append(
            "AC6: test_tenant_isolation_enforcement.cpp missing AC9 for #3409 (no design docs / no test_issue_*)"
        )

    # AC7 — build.py coverage gate registration.
    if "check_grant_ssot_ta_fence_3409" not in build:
        fails.append(
            "AC7: build.py does not register check_grant_ssot_ta_fence_3409 (linter not wired into the coverage gate)"
        )

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1

    print("PASS: #3409 CapabilityRegistry::grant SSOT TA fence contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
