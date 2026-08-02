#!/usr/bin/env python3
"""Issue #2491: mandate TenantScope at fiber spawn/resume entry
(no residual principal). assigned_tenant_id_ on Fiber + bridge hooks
aura_fiber_install_tenant_scope_for_resume / aura_fiber_release_tenant_scope_after_yield
on Fiber::resume / yield boundary.

Contract:
  AC1 Fiber.assigned_tenant_id accessor + storage field
  AC2 resume installs TenantScope before swapcontext; release after yield
  AC3 Cross-tenant mutate without cross-grant → IsolationDeny
  AC4 Nested Scope release() restores outer principal
  AC5 Off sandbox skips force (Soft unit path unchanged)
  AC6 Multi-tenant stress: no cross-tenant principal bleed
  AC7 Source-cite + tests + CMake + build.py gate + this linter

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    fb = _read("src/compiler/fiber_bridge.cpp")
    em = _read("src/compiler/evaluator_fiber_mutation.cpp")
    om = _read("src/compiler/observability_metrics.h")
    test = _read("tests/compiler/test_tenant_scope_fiber_mandate_2491.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — Fiber assigned_tenant_id field + accessors.
    must("Issue #2491", "AC1", fh)
    must("assigned_tenant_id_", "AC1", fh)
    must("set_assigned_tenant_id", "AC1", fh)
    must("assigned_tenant_id()", "AC1", fh)
    must("static_tenant_scope_mismatch_total_", "AC1", fh)
    must("bump_tenant_scope_mismatch", "AC1", fh)
    must("ac1_assigned_tenant_id_accessor", "AC1", test)

    # AC2 — bridge hooks wired in Fiber::resume (install before swapcontext,
    # release after yield).
    must("aura_fiber_install_tenant_scope_for_resume", "AC2", fh)
    must("aura_fiber_release_tenant_scope_after_yield", "AC2", fh)
    must("aura_fiber_install_tenant_scope_for_resume", "AC2", fb)
    must("aura_fiber_release_tenant_scope_after_yield", "AC2", fb)
    must("Issue #2491", "AC2", fb)
    must("aura_fiber_install_tenant_scope_for_resume(this)", "AC2", fc)
    must("aura_fiber_release_tenant_scope_after_yield()", "AC2", fc)
    must("ac2_resume_reinstalls_and_release_restores", "AC2", test)

    # AC3 — strong bridge def installs scope; mismatch bumps metric;
    # isolation path remains single authority via #2490 require_effect.
    must("Issue #2491", "AC3", em)
    must("aura_fiber_install_tenant_scope_for_resume", "AC3", em)
    must("g_fiber_tenant_scope", "AC3", em)
    must("TenantScope", "AC3", em)
    must("ac3_cross_tenant_isolation_deny", "AC3", test)

    # AC4 — TenantScope release() restores prev (existing behavior;
    # covered by the existing #2055 / #2385 tests + this AC4 row).
    must("ac4_nested_reentry_preserves_outer", "AC4", test)

    # AC5 — Off sandbox short-circuit.
    must("mode == 0", "AC5", em)
    must("ac5_off_sandbox_no_force", "AC5", test)

    # AC6 — multi-tenant stress loop in test.
    must("ac6_multi_tenant_stress_no_bleed", "AC6", test)

    # AC7 — registrations + observability metric.
    must("tenant_scope_mismatch_total", "AC7", om)
    must("ac7_source_and_gate", "AC7", test)
    must("Issue #2491", "AC7", em)
    must("test_tenant_scope_fiber_mandate_2491", "AC7", cmake)
    must("aura_add_issue_test(test_tenant_scope_fiber_mandate_2491)", "AC7", cmake)
    must("aura_issue_test_link_light(test_tenant_scope_fiber_mandate_2491)", "AC7", cmake)
    must("check_tenant_scope_fiber_mandate_2491", "AC7", build)
    must("cmd_tenant_scope_fiber_mandate_2491_coverage", "AC7", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2491 TenantScope fiber mandate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
