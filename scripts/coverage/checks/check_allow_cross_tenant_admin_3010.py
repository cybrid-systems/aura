#!/usr/bin/env python3
"""Issue #3010: allow_cross_tenant_ write requires TenantAdmin.

Contract (one row per AC):
  AC1  Under production (Restricted/Strict: sandbox_mode_ != 0 ||
       effect_sandbox_mode() != 0), security:set-tenant-principal! with
       allow_cross=#t requires explicit TenantAdmin via
       effects_for_locked (#3492: wildcard-only is not privilege).
       Missing → #f + SE EffectDeny reason
       'allow-cross-needs-tenant-admin' + deny counter
       (allow_cross_tenant_deny_total). Flag stays false.
  AC2  Restricted + TenantAdmin can set the flag. Wildcard-only
       cannot (#3492 / #3411). Subsequent isolation bypass is the
       flag's job; #2968 grant-write remains independently gated.
  AC3  Soft / Off (sandbox_mode_ == 0 && effect_sandbox_mode() == 0)
       short-circuits: zero added cost, no privilege lookup, flag can
       still be set.
  AC4  C++ Evaluator::set_tenant_principal hardens the same gate (host
       cannot bypass the EDSL check). Tenant id still binds on deny.
  AC5  Additive query keys (schema-3010): allow-cross-tenant-admin-wired
       + allow-cross-tenant-deny-total; snapshot exposes
       allow_cross_tenant_deny.
  AC6  Source-cite; extend test_tenant_isolation_enforcement (#81967);
       no test_issue_3010.cpp; no docs/design/ (#1655).

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
    ixx = _read("src/compiler/evaluator.ixx")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # ── AC1: EDSL prim privilege test precedes set_tenant_principal ─
    must("Issue #3010", "AC1", posture)
    must("allow-cross-needs-tenant-admin", "AC1", posture)
    must("allow_cross_tenant_deny_total", "AC1", posture)
    must("force_bind", "AC1", posture)
    must("effects_for_locked", "AC1 locked TA face", posture)
    must("holds_wildcard_only_locked", "AC1 wildcard-only deny (#3492)", posture)
    must("kEffectTenantAdmin", "AC1", posture)
    must("kCapTenantAdmin", "AC1", sec_caps)
    must("kCapWildcard", "AC1", sec_caps)
    prim_mark = posture.find('add("security:set-tenant-principal!"')
    next_add = posture.find('add("', prim_mark + 4) if prim_mark >= 0 else -1
    prim_body = posture[prim_mark:next_add] if prim_mark >= 0 and next_add > prim_mark else ""
    if not prim_body:
        fails.append("AC1: security:set-tenant-principal! prim body not found")
    else:
        priv_p = prim_body.find("effects_for_locked")
        wild_p = prim_body.find("holds_wildcard_only_locked")
        set_p = prim_body.find("ev.set_tenant_principal(")
        deny_p = prim_body.find("allow-cross-needs-tenant-admin")
        if priv_p < 0 or set_p < 0 or priv_p > set_p:
            fails.append("AC1: prim privilege test must precede ev.set_tenant_principal")
        if wild_p < 0 or set_p < 0 or wild_p > set_p:
            fails.append("AC1: holds_wildcard_only_locked must precede ev.set_tenant_principal")
        if deny_p < 0 or set_p < 0 or deny_p > set_p:
            fails.append("AC1: prim SE reason allow-cross-needs-tenant-admin must precede set")
        if "return make_bool(false)" not in prim_body:
            fails.append("AC1: prim must return #f on missing privilege")
        if "has_capability(kCapWildcard)" in prim_body:
            fails.append("AC1: prim must not treat kCapWildcard as elevate/allow_cross (#3492)")

    # ── AC2: TenantAdmin allow path still calls set_tenant_principal ─
    must("kEffectTenantAdmin", "AC2", posture)
    must("holds_wildcard_only_locked", "AC2 wildcard-only closed", posture)

    # ── AC3: Soft/Off zero-cost short-circuit ──────────────────────
    must("force_bind", "AC3", posture)
    must("force_bind", "AC3", sec)
    must("effect_sandbox_mode()", "AC3", sec)

    # ── AC4: C++ set_tenant_principal hardens the same gate ────────
    must("Issue #3010", "AC4", sec)
    must("allow-cross-needs-tenant-admin", "AC4", sec)
    must("has_capability(kCapTenantAdmin)", "AC4", sec)
    must("allow_cross_tenant_deny_total", "AC4", sec)
    must("allow_cross_tenant()", "AC4", ixx)
    deny_cpp = sec.find("allow_cross_tenant_deny_total.fetch_add")
    assign_flag = sec.rfind("allow_cross_tenant_ = allow_cross")
    if deny_cpp < 0 or assign_flag < 0 or deny_cpp > assign_flag:
        fails.append("AC4: C++ deny counter must precede allow_cross_tenant_ = allow_cross")

    # ── AC5: additive query keys + snapshot ────────────────────────
    must("schema-3010", "AC5", posture)
    must("issue-3010", "AC5", posture)
    must("allow-cross-tenant-admin-wired", "AC5", posture)
    must("allow-cross-tenant-deny-total", "AC5", posture)
    must("allow_cross_tenant_deny", "AC5", iso)
    must("allow_cross_tenant_deny_total", "AC5", iso)

    # ── AC6: source-cite + tests + no invent + no docs/design/ ─────
    must("#3010", "AC6", iso)
    must("#3010", "AC6", sec)
    must("#3010", "AC6", test_iso)
    must("schema-3010", "AC6", posture)
    must("check_allow_cross_tenant_admin_3010", "AC6", build)
    for rel in ("tests/core/test_issue_3010.cpp", "tests/compiler/test_issue_3010.cpp"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #81967")
    for rel in (
        "docs/design/3010-allow-cross-tenant-admin.md",
        "docs/design/3010-*.md",
    ):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #3010 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3010 allow_cross_tenant TenantAdmin gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
