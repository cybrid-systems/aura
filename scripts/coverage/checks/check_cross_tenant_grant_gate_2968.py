#!/usr/bin/env python3
"""Issue #2968: cross-tenant grant write path requires TenantAdmin.

Contract (one row per AC):
  AC1  Under production (Restricted/Strict: sandbox_mode_ != 0 ||
       effect_sandbox_mode() != 0), Evaluator::grant_cross_tenant_access
       requires the caller to hold TenantAdmin (or the "tenant-admin" /
       "capability" string caps that map to it). Missing → deny + SE
       EffectDeny reason 'cross-tenant-grant-needs-tenant-admin' + deny
       counter (cross_tenant_grant_deny_total). No grant written, no
       allow-counter bump.
  AC2  Foreign-tenant grant_effect_capability (tenant_id != 0 &&
       tenant_id != capability_tenant_id_) under production requires the
       same TenantAdmin gate; same-tenant self-grant stays on the existing
       Mutate/capability policy (documented in source-cite).
  AC3  Soft / Off path (sandbox_mode_ == 0 && effect_sandbox_mode() == 0)
       short-circuits: zero added cost, no privilege lookup, no deny.
  AC4  Grant success still bumps cross_tenant_capability_grant_total only
       on allow; existing isolation check metrics + dual-write SE unchanged.
  AC5  Additive query keys (schema-2968): cross-tenant-grant-tenant-admin-
       wired + cross-tenant-grant-deny-total; snapshot exposes
       cross_tenant_grant_deny (TenantIsolationStatsSnapshot).
  AC6  Source-cite in workspace_isolation.hh + evaluator_security.cpp +
       evaluator_primitives_security.cpp; tests extend the workspace
       isolation suite per #81967 (no new test_issue_NNNN.cpp); no
       docs/design per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    iso = _read("src/core/workspace_isolation.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    sec_caps = _read("src/compiler/security_capabilities.h")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # ── AC1: TenantAdmin gate on grant_cross_tenant_access ─────────
    must("Issue #2968", "AC1", sec)
    must("cross-tenant-grant-needs-tenant-admin", "AC1", sec)
    must("cross_tenant_grant_deny_total", "AC1", iso)
    must("cross_tenant_grant_deny_total", "AC1", sec)
    must("force_bind", "AC1", sec)
    must("has_capability(kCapTenantAdmin)", "AC1", sec)
    must("kCapTenantAdmin", "AC1", sec_caps)
    must("kCapCapability", "AC1", sec_caps)
    # Deny must precede the policy grant call (use call-site order, not
    # comment mentions — the deny SE emit + counter bump come before
    # grant_cross_tenant).
    deny_p = sec.find("cross_tenant_grant_deny_total.fetch_add")
    grant_p = sec.find("g_workspace_isolation().grant_cross_tenant(")
    if deny_p < 0 or grant_p < 0 or deny_p > grant_p:
        fails.append("AC1: deny counter fetch_add must precede grant_cross_tenant call in grant_cross_tenant_access")

    # ── AC2: foreign-tenant grant_effect_capability gate ───────────
    must("foreign_target", "AC2", sec)
    must("capability_tenant_id_", "AC2", sec)
    must("cross-tenant-grant-needs-tenant-admin", "AC2", sec)
    must("Issue #2968 AC2", "AC2", sec)

    # ── AC3: Soft/Off zero-cost short-circuit ──────────────────────
    must("force_bind", "AC3", sec)

    # ── AC4: allow counter only on allow; check metrics unchanged ──
    must("cross_tenant_capability_grant_total", "AC4", iso)
    must("cross_tenant_capability_deny_total", "AC4", iso)

    # ── AC5: additive query keys + snapshot ────────────────────────
    must("schema-2968", "AC5", posture)
    must("issue-2968", "AC5", posture)
    must("cross-tenant-grant-tenant-admin-wired", "AC5", posture)
    must("cross-tenant-grant-deny-total", "AC5", posture)
    must("cross_tenant_grant_deny", "AC5", iso)  # snapshot field

    # ── AC6: source-cite + tests + no invent + no docs/design/ ─────
    must("#2968", "AC6", iso)
    must("#2968", "AC6", sec)
    must("#2968", "AC6", test_iso)
    must("schema-2968", "AC6", posture)
    must("check_cross_tenant_grant_gate_2968", "AC6", build)  # linter wired
    for rel in ("tests/core/test_issue_2968.cpp", "tests/compiler/test_issue_2968.cpp"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #81967")
    for rel in ("docs/design/2968-cross-tenant-grant-gate.md", "docs/design/2968-*.md"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #2968 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #2968 cross-tenant grant TenantAdmin gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
