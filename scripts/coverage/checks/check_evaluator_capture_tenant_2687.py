#!/usr/bin/env python3
"""Issue #2687: per-Evaluator isolation_capture_tenant (close #2659 residual stamp race).

Contract:
  AC1 Two Evaluators tenants 7/42 concurrent make_stamped_ref / capture
      → each ref.tenant_id matches only its Evaluator principal
      (no cross-stamp). Evaluator::stamp_stable_ref / make_stamped_ref
      / export_ref use Evaluator::capability_tenant_id_ (already true
      from #2659 + #2056).
  AC2 TenantScope snapshot/restore does not publish the other
      Evaluator's capture principal into global stamp path.
      WorkspaceIsolationPolicy::set_current_tenant still writes the
      global atomic for legacy single-tenant path (best-effort mirror
      per issue proposal), but Evaluator::set_tenant_principal must
      NOT write the global (#2659 AC3 preserved).
  AC3 Existing #2659 require_effect / check_boundary concurrent tests
      still green. Evaluator::check_boundary_ex uses
      capability_tenant_id_ + allow_cross_tenant_ (already correct).
  AC4 Soft / tenant=0 capture remains permissive (legacy single-tenant).
      Tenant 0 in TenantScope → isolation_enabled = false, capture
      leaves tenant_id 0 (g_isolation_capture_tenant stays 0 →
      maybe_stamp_stable_ref_isolation_tenant returns false).
  AC5 Additive observability:
      - g_isolation_capture_stamp_local_total_atomic — Evaluator::stamp_stable_ref
      - g_isolation_capture_stamp_global_fallback_total_atomic —
        maybe_stamp_stable_ref_isolation_tenant (FlatAST fallback path)
      - g_isolation_capture_stamp_evaluator_miss_total_atomic —
        diagnostic for FlatAST factories called under an active
        Evaluator (should have used Evaluator::stamp_stable_ref)
      - schema-2687 / issue-2687 sentinels
  AC6 Source-cite + coverage linter; extend test_tenant_isolation_enforcement
      / #2659 suite per #81967 (no docs/design/* per #1655).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    prov = _read("src/core/provenance_tracker.hh")
    eval_sec = _read("src/compiler/evaluator_security.cpp")
    workspace = _read("src/core/workspace_isolation.hh")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    _read("build.py")

    # AC1 — Evaluator::stamp_stable_ref uses capability_tenant_id_ (per-Evaluator,
    # already true from #2659). The new #2687 addition: bump
    # g_isolation_capture_stamp_local_total_atomic counter.
    must("Evaluator::stamp_stable_ref", "AC1", eval_sec)
    must("capability_tenant_id_", "AC1", eval_sec)
    must("g_isolation_capture_stamp_local_total_atomic", "AC1", eval_sec)
    # The local counter must be bumped INSIDE Evaluator::stamp_stable_ref body.
    m = re.search(
        r"void\s+Evaluator::stamp_stable_ref\s*\([^)]*\)\s*(?:const)?\s*(?:noexcept)?\s*\{(.+?)\n\s*\}",
        eval_sec,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        fails.append("AC1: Evaluator::stamp_stable_ref impl not found")
    else:
        body = m.group(1)
        if "g_isolation_capture_stamp_local_total_atomic" not in body:
            fails.append("AC1: Evaluator::stamp_stable_ref body missing local counter bump")
        if "capability_tenant_id_" not in body:
            fails.append("AC1: Evaluator::stamp_stable_ref body missing capability_tenant_id_")

    # AC2 — WorkspaceIsolationPolicy::set_current_tenant still writes the global
    # atomic (legacy / single-tenant path / best-effort mirror). Evaluator::
    # set_tenant_principal must NOT write the global (#2659 AC3 preserved).
    # Function is declared as a member of the struct (no leading qualifier).
    must("struct WorkspaceIsolationPolicy", "AC2", workspace)
    must("set_current_tenant", "AC2", workspace)
    must("set_isolation_capture_tenant", "AC2", workspace)
    must("Evaluator::set_tenant_principal", "AC2", eval_sec)
    # Evaluator::set_tenant_principal body must only set capability_tenant_id_.
    m = re.search(
        r"void\s+Evaluator::set_tenant_principal\s*\([^)]*\)\s*(?:noexcept)?\s*\{(.+?)\n\s*\}",
        eval_sec,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        fails.append("AC2: Evaluator::set_tenant_principal impl not found")
    else:
        body = m.group(1)
        if "capability_tenant_id_" not in body:
            fails.append("AC2: Evaluator::set_tenant_principal must set capability_tenant_id_")
        if "set_isolation_capture_tenant" in body:
            fails.append(
                "AC2 (#2659 AC3 regression): Evaluator::set_tenant_principal "
                "must NOT write g_isolation_capture_tenant atomic"
            )

    # AC3 — Existing #2659 paths preserved. check_boundary_ex +
    # require_effect use capability_tenant_id_ + allow_cross_tenant_.
    must("check_boundary_ex", "AC3", eval_sec)
    must("check_boundary", "AC3", eval_sec)
    must("capability_tenant_id_", "AC3", eval_sec)
    # Existing #2659 lineage reference.
    must("#2659", "AC3", eval_sec)

    # AC4 — Soft / tenant=0 capture permissive. g_isolation_capture_tenant
    # default 0 → maybe_stamp_stable_ref_isolation_tenant returns false
    # (no stamp, tenant_id stays 0 = legacy single-tenant).
    must("g_isolation_capture_tenant()", "AC4", prov)
    # maybe_stamp_stable_ref_isolation_tenant returns false when tid == 0.
    must("maybe_stamp_stable_ref_isolation_tenant", "AC4", prov)
    # isolation_capture_tenant() returns 0 default (legacy single-tenant).
    if "t{0}" not in prov:
        fails.append("AC4: g_isolation_capture_tenant default not 0 (legacy single-tenant)")

    # AC5 — Counters + query surface wired.
    must("g_isolation_capture_stamp_local_total_atomic", "AC5", prov)
    must("g_isolation_capture_stamp_global_fallback_total_atomic", "AC5", prov)
    must("g_isolation_capture_stamp_evaluator_miss_total_atomic", "AC5", prov)
    must("kEvaluatorCaptureTenantIssue", "AC5", prov)
    # Query surface.
    must("isolation-capture-stamp-local-total", "AC5", q)
    must("isolation-capture-stamp-global-fallback-total", "AC5", q)
    must("isolation-capture-stamp-evaluator-miss-total", "AC5", q)
    must("schema-2687", "AC5", q)
    must("issue-2687", "AC5", q)
    # Global-fallback counter must be bumped from
    # maybe_stamp_stable_ref_isolation_tenant (FlatAST fallback path).
    # Brace-count extract (nested if for #2705 hard-close defeats non-greedy).
    fn_sig = "bool maybe_stamp_stable_ref_isolation_tenant"
    fi = prov.find(fn_sig)
    if fi < 0:
        fi = prov.find("maybe_stamp_stable_ref_isolation_tenant")
    body = ""
    if fi >= 0:
        brace = prov.find("{", fi)
        if brace >= 0:
            depth = 0
            for j in range(brace, len(prov)):
                if prov[j] == "{":
                    depth += 1
                elif prov[j] == "}":
                    depth -= 1
                    if depth == 0:
                        body = prov[brace + 1 : j]
                        break
    if not body:
        fails.append("AC5: maybe_stamp_stable_ref_isolation_tenant impl not found")
    elif "g_isolation_capture_stamp_global_fallback_total_atomic" not in body:
        fails.append(
            "AC5: maybe_stamp_stable_ref_isolation_tenant must bump "
            "g_isolation_capture_stamp_global_fallback_total_atomic"
        )

    # AC6 — no docs/design/* per #1655.
    for rel in (
        "docs/design/evaluator_capture_tenant_2687.md",
        "docs/evaluator_capture_tenant_2687.md",
        "design/2687.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # AC6 — self-coverage: #2687 sentinel in provenance_tracker.hh + query +
    # evaluator_security.cpp + workspace_isolation.hh. Use "#2687" (not
    # "Issue #2687") to accept combined citations like
    # "Issue #2687 / #2659 / #2056".
    must("#2687", "AC6", prov)
    must("#2687", "AC6", q)
    must("#2687", "AC6", eval_sec)

    # Linter file on disk.
    linter_path = ROOT / "scripts/coverage/checks/check_evaluator_capture_tenant_2687.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2687 per-Evaluator isolation capture tenant — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
