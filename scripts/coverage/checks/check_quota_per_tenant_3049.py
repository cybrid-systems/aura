#!/usr/bin/env python3
"""Issue #3049: per-tenant ResourceQuota under multi-tenant production.

Contract (one row per AC):
  AC1  reserve/consume can key by TenantId for fibers + mutations
  AC2  tenant A exhaust does not deny tenant B (process ceiling still binds)
  AC3  Soft / Off / per-tenant off: process-global path, no map lookup
  AC4  additive metrics (rejects_total stays; quota_reject_by_tenant_*)
  AC5  deny reason quota-exceeded:tenant=N:dim=*
  AC6  source-cite resource_quota.hh + orch/fiber admission; extend
       quota / isolation suite; no test_issue_3049.cpp; no docs/design/

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

    rq = _read("src/core/resource_quota.hh")
    sched = _read("src/serve/scheduler.cpp")
    orch = _read("src/orch/agent_spawn.h")
    porc = _read("src/serve/parallel_orch.h")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    must("kQuotaPerTenantIssue", "AC1", rq)
    must("TenantId tenant", "AC1", rq)
    must("check_and_consume_tenant", "AC1", rq)
    must("set_tenant_limit", "AC1", rq)
    must("quota_per_tenant_enabled", "AC1", rq)
    must("AURA_QUOTA_PER_TENANT", "AC1", rq)
    must("AURA_MULTI_TENANT", "AC1", rq)

    must("check_and_consume_fiber(spawn_tenant)", "AC1", sched)
    must("quota_tenant_id", "AC1", sched)
    must("check_orchestration_fibers", "AC1", orch)
    must("orch_tenant", "AC1", orch)
    must("current_quota_tenant", "AC1", porc)

    must("quota-exceeded:tenant=", "AC5", rq)
    must("quota_reject_by_tenant_total", "AC4", rq)
    must("rejects_total", "AC4", rq)
    must("schema-3049", "AC4", obs)
    must("quota-reject-by-tenant-total", "AC4", obs)
    must("quota-per-tenant-wired", "AC4", obs)

    must("quota_per_tenant_enabled", "AC3", rq)
    must("AURA_SANDBOX", "AC3", rq)

    must("3049", "AC6", test_iso)
    must("quota-exceeded:tenant=1:dim=fibers", "AC2", test_iso)
    must("check_quota_per_tenant_3049", "AC6", build)
    if (ROOT / "tests" / "core" / "test_issue_3049.cpp").is_file():
        fails.append("AC6: test_issue_3049.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3049-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3049 per-tenant ResourceQuota")
    return 0


if __name__ == "__main__":
    sys.exit(main())
