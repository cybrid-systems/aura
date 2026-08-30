#!/usr/bin/env python3
"""Issue #3434: production spawn stamps Fiber::assigned_tenant_id.

The TenantScope resume mandate (#2491) was test-only: every caller of
Fiber::set_assigned_tenant_id lived under tests/, so the strong resume
hook aura_fiber_install_tenant_scope_for_resume hit "assigned == 0 →
return" on production orch spawn. Scope install / principal mismatch /
steal-session revoke stayed dark on the real spawn path.

This issue resolves the tenant at spawn (spec.tenant_id → parent fiber
assigned_tenant_id → quota TLS tenant), stamps it on the fiber, and
denies spawn ("tenant-required") when production Restricted+MT / Strict
cannot resolve a tenant. Soft/Off and Restricted single-tenant keep the
legacy zero-cost path.

Contract:
  AC1  production orch/Scheduler spawn (Restricted+MT or Strict) writes
       fiber.assigned_tenant_id != 0 without the test harness calling
       set_assigned_tenant_id
  AC2  steal x resume: fiber assigned=7, worker Evaluator principal=9 →
       TenantScope rebinds to 7; tenant_scope_mismatch_hard +
       IsolationDeny fiber-principal-mismatch (resume hook, #2839/#2883)
  AC3  session_bound grants on stolen mid revoked on resume
       (revoke_session_grants_on_steal_or_abort_locked) when assigned != 0
  AC4  Soft/Off + assigned=0: resume still no-op, zero extra lock
  AC5  Restricted single-tenant (no AURA_MULTI_TENANT): no new deny on
       spawn if host leaves tenant 0 (legacy REPL)
  AC6  extend test_tenant_scope_fiber_mandate.cpp (no test_issue_*.cpp,
       no docs/design/*, no new query key)

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

    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    hook = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_tenant_scope_fiber_mandate.cpp")
    build = _read("build.py")

    # AC1: production spawn resolves tenant + stamps the fiber.
    must("Issue #3434", "AC1 stamp", spawn)
    must("spawn_tenant = spec.tenant_id", "AC1 spec tenant source", spawn)
    must("serve::g_current_fiber->assigned_tenant_id()", "AC1 parent fallback", spawn)
    must("f->set_assigned_tenant_id(spawn_tenant)", "AC1 stamp on fiber", spawn)
    must("spawn_tenant_required_total", "AC1 additive counter", spawn)
    must("ac3434_1_production_spawn_stamps_tenant", "AC1 test", test)

    # AC2: resume hook rebind + hard mismatch surface (already built).
    must("aura_fiber_install_tenant_scope_for_resume", "AC2 hook", hook)
    must("TenantScope(*ev, assigned", "AC2 rebind to assigned", hook)
    must("tenant_scope_mismatch_hard", "AC2 hard mismatch metric", hook)
    must("fiber-principal-mismatch", "AC2 IsolationDeny reason", hook)
    must("ac3434_2_steal_resume_rebind", "AC2 test", test)

    # AC3: session revoke on stolen mid when assigned != 0 (resume hook).
    must("revoke_session_grants_on_steal_or_abort_locked", "AC3 revoke", hook)
    must("has_resume_safety_ticket() && f->session_mid() != 0", "AC3 stolen-mid gate", hook)
    must("ac3434_3_session_revoke_on_resume", "AC3 test", test)

    # AC4: Soft/Off + assigned=0 resume no-op (zero extra lock).
    must("const auto assigned = f->assigned_tenant_id();", "AC4 assigned read", hook)
    must("if (assigned == 0)", "AC4 no-op", hook)
    must("ac3434_4_soft_zero_cost", "AC4 test", test)

    # AC5: Restricted single-tenant (no AURA_MULTI_TENANT) → no deny.
    must("multi_tenant_env_active()", "AC5 MT gate", spawn)
    must("aura::core::sandbox::is_strict()", "AC5 strict gate", spawn)
    must("tenant_required_gate && spawn_tenant == 0", "AC5 deny condition", spawn)
    must("ac3434_5_restricted_single_tenant_no_deny", "AC5 test", test)

    # AC6: extend test_tenant_scope_fiber_mandate; linter wired; no invent.
    must("ac3434_6_source_and_linter", "AC6 test", test)
    must("check_tenant_spawn_mandate_3434", "AC6 build.py", build)
    must("spec.tenant_id = tenant_id != 0 ? tenant_id : ev.capability_tenant_id()", "AC6 prim wiring", prim)
    if (ROOT / "tests" / "compiler" / "test_issue_3434.cpp").is_file():
        fails.append("AC6: test_issue_3434.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3434.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3434.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3434-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    if "class AgentRegistry" in spawn or "struct AgentRegistry" in spawn:
        fails.append("AC6: process-global AgentRegistry present (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3434 tenant spawn mandate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
