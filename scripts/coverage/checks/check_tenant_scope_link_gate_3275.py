#!/usr/bin/env python3
"""Issue #3275: production link gate for the tenant-scope resume ABI.

Residual of #2491/#2839: the weak no-op
aura_fiber_install_tenant_scope_for_resume / release in fiber_bridge.cpp
can resolve when the Evaluator module is NOT linked (slim / custom
production link sets: serve + orch, no evaluator partition). Fiber resumes
then run under the worker's ambient capability_tenant_id_ — assigned_tenant
is never rebound, silently skipping principal isolation. #3275 closes the
link-configuration residual without touching the TenantScope body:

  AC1  weak bodies are production-aware: under the production lock they
       abort (fail-closed, #2377 pattern) instead of silently returning;
       Soft / AURA_SANDBOX=off / light-link bump the additive
       tenant_scope_resume_missing_total counter and no-op (contract)
  AC2  strong-identity marker aura_abi_strong_tenant_scope_resume_v():
       weak stub returns 0, strong def (evaluator_fiber_mutation.cpp)
       returns 1
  AC3  startup self-check (runtime_production_abi.cpp) requires the marker
       == 1 under production (new fail bit 6) in BOTH the single-worker
       and multi-worker variants — mis-linked production fails at startup
       (link / startup gate), never silent ambient principal
  AC4  Soft / sandbox=off / unit light-link unchanged (self-check not
       required; weak no-op preserved)
  AC5  tests extend test_tenant_scope_fiber_mandate.cpp (#81967);
       no test_issue_3275.cpp; no docs/design/ (#1655); build.py wires
       linter

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

    fb = _read("src/compiler/fiber_bridge.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    rab = _read("src/serve/runtime_production_abi.cpp")
    rah = _read("src/serve/runtime_production_abi.h")
    gc = _read("src/core/gc_hooks.h")
    test = _read("tests/compiler/test_tenant_scope_fiber_mandate.cpp")
    build = _read("build.py")

    must("aura_abi_strong_tenant_scope_resume_v", "AC1 weak marker", fb)
    must("aura_abi_strong_tenant_scope_resume_v", "AC2 strong def", fm)
    must("return 1", "AC2 strong returns 1", fm)
    must("steal_snapshot_soft_production_locked()", "AC1 lock-aware weak", fb)
    must("std::abort()", "AC1 fail-closed abort", fb)
    must("bump_tenant_scope_resume_missing_total", "AC1 soft counter", fb)
    must("g_tenant_scope_resume_missing_total", "AC1 counter def", gc)
    must("kProductionAbiSelfcheckFailBitTenantScope", "AC3 fail bit", rah)
    must("aura_abi_strong_tenant_scope_resume_v", "AC3 self-check", rab)
    if "aura_abi_strong_tenant_scope_resume_v() == 0" not in rab:
        fails.append("AC3: marker check missing in runtime_production_abi.cpp")
    must("ac3275_1_link_gate_source_cite", "AC5 test", test)
    must("ac3275_2_production_lock_roundtrip", "AC5 test", test)
    must("ac3275_3_soft_no_abort_path", "AC5 test", test)
    must("ac3275_4_linter_and_no_invent", "AC5 test", test)
    must("check_tenant_scope_link_gate_3275", "AC5 build.py", build)
    if _read("tests/compiler/test_issue_3275.cpp"):
        fails.append("AC5: test_issue_3275.cpp present (forbidden #81967)")
    if _read("docs/design/3275-tenant-scope-link-gate.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3275 tenant_scope_link_gate:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3275 tenant_scope_link_gate: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
