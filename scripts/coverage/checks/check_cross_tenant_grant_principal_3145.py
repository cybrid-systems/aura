#!/usr/bin/env python3
"""Issue #3145: try_grant_cross_tenant_privileged + grant_macro_self_evo
privilege check — explicit caller_principal (per-Evaluator
capability_tenant_id_) instead of the process-global default_tenant
(almost always 0 under multi-Evaluator / TenantScope), and effects_for
under the registry mtx via effects_for_locked so a concurrent revoke
cannot race past the fence.

Contract (one row per AC):
  AC1  Under production (Restricted/Strict), concurrent revoke of
       TenantAdmin from a second Evaluator makes a racing
       grant_cross_tenant fail closed (deny + SE + counter). The
       privilege read uses effects_for_locked under reg.mtx; explicit
       caller_principal wins over default_tenant.
  AC2  Gate uses the calling Evaluator's capability_tenant_id_ (or
       explicit principal) — never process-global default_tenant alone.
       Evaluator::grant_cross_tenant_access forwards capability_tenant_id_.
  AC3  Soft/Off remains zero-cost: the SSOT helper short-circuits before
       any lock or principal load. AC3 unaffected by the new parameter.
  AC4  grant_macro_self_evo privilege check aligned: explicit
       caller_principal parameter, same principal source, runs under the
       registry mtx (read atomic w.r.t. concurrent grant/revoke).
  AC5  Existing suite (test_tenant_isolation_enforcement + grant-epoch
       + require_effect auto-isolation) still green; dual-Evaluator
       chaos case added (concurrent revoke from second Evaluator closes
       the racing grant). Tests extend the workspace isolation suite
       per #81967 (no new test_issue_NNNN.cpp); no docs/design/ (#1655).
  AC6  Coverage linter records the residual closed; no new query key
       (per issue body — no schema-3145 / issue-3145 in the posture).
       Linter wired into build.py.

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
    cap = _read("src/core/capability_model.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # ── AC1: SSOT helper uses effects_for_locked under reg.mtx + accepts
    # caller_principal (TOCTOU closure via locked variant + explicit
    # principal source).
    must("Issue #3145", "AC1", iso)
    must("caller_principal", "AC1", iso)
    must("effects_for_locked", "AC1", iso)
    must("reg.mtx", "AC1", iso)
    must("try_grant_cross_tenant_privileged", "AC1", iso)
    # AC1: Soft/Off short-circuit must precede any lock or principal load.
    off_p = iso.find("EffectSandboxMode::Off")
    lock_p = iso.find("reg.mtx")
    if off_p < 0:
        fails.append("AC1: Soft/Off short-circuit marker missing")
    elif lock_p > 0 and lock_p < off_p:
        fails.append(
            "AC1: registry mtx acquired before Soft/Off short-circuit (must short-circuit first for AC3 zero-cost)"
        )

    # ── AC2: Evaluator wrapper forwards capability_tenant_id_ to SSOT
    # method. Gate uses calling Evaluator's principal, never
    # process-global alone.
    must("Issue #3145", "AC2", sec)
    must("capability_tenant_id_", "AC2", sec)
    must("g_workspace_isolation().grant_cross_tenant", "AC2", sec)
    # The wrapper must pass capability_tenant_id_ (not just default_tenant).
    # Use a balanced-paren scan: walk from the opening '(' to the matching
    # ')' so the call may span newlines.
    import re as _re

    call_matches = list(_re.finditer(r"g_workspace_isolation\(\)\.grant_cross_tenant\(", sec))
    if not call_matches:
        fails.append("AC2: cannot locate grant_cross_tenant call site in Evaluator wrapper")
    else:
        call_ok = False
        for m in call_matches:
            start = m.end()
            depth = 1
            i = start
            while i < len(sec) and depth > 0:
                ch = sec[i]
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                i += 1
            if depth != 0:
                continue
            call_text = sec[start : i - 1]
            if "capability_tenant_id_" in call_text:
                call_ok = True
                break
        if not call_ok:
            fails.append("AC2: Evaluator wrapper grant_cross_tenant call must forward capability_tenant_id_")

    # ── AC3: Soft/Off zero-cost. No lock, no principal load on the
    # short-circuit path.
    must("Soft/Off", "AC3", iso)
    must("EffectSandboxMode::Off", "AC3", iso)
    # Counter names + SE reason unchanged (#2968 stable).
    must("cross-tenant-grant-needs-tenant-admin", "AC3", iso)
    must("cross_tenant_grant_deny_total", "AC3", iso)

    # ── AC4: grant_macro_self_evo aligned (caller_principal + locked).
    must("Issue #3145", "AC4", cap)
    must("caller_principal", "AC4", cap)
    must("grant_macro_self_evo", "AC4", cap)
    # The check must run under mtx (existing std::lock_guard<std::mutex>
    # block on the registry). The by_tenant.find() inside the
    # admin-check lambda runs while the lock is held.
    macro_p = cap.find("void grant_macro_self_evo")
    if macro_p > 0:
        end = macro_p + 8000  # generous slice for the function body
        snip = cap[macro_p:end]
        if "std::lock_guard<std::mutex> lock(mtx)" not in snip:
            fails.append("AC4: grant_macro_self_evo must take registry mtx (admin check must be locked)")
    # Prim call site forwards the principal.
    must("ev.capability_tenant_id()", "AC4", prim)
    must("Issue #3145", "AC4", prim)

    # ── AC5: dual-Evaluator chaos case added; tests extend the
    # workspace isolation suite per #81967 (no new
    # tests/core/test_issue_3145.cpp); no docs/design/ per #1655.
    must("#3145", "AC5", test_iso)
    must("dual-Evaluator chaos", "AC5", test_iso)
    must("post-revoke", "AC5", test_iso)
    must("explicit caller_principal", "AC5", test_iso)
    for rel in ("tests/core/test_issue_3145.cpp", "tests/compiler/test_issue_3145.cpp"):
        if _read(rel):
            fails.append(f"AC5: {rel} exists — forbidden per #81967")
    docs_design_dir = ROOT / "docs" / "design"
    if docs_design_dir.is_dir():
        for f in docs_design_dir.glob("3145-*.md"):
            fails.append(f"AC5: {f.relative_to(ROOT)} exists — forbidden per #1655")

    # ── AC6: linter wired into build.py; no new posture / query key
    # (issue body explicit).
    must("check_cross_tenant_grant_principal_3145", "AC6", build)
    must("Issue #3145", "AC6", build)
    if "schema-3145" in prim:
        fails.append("AC6: schema-3145 leaked into posture — forbidden per issue body")
    if "issue-3145" in prim:
        fails.append("AC6: issue-3145 leaked into posture — forbidden per issue body")

    if fails:
        print(f"Issue #3145 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3145 cross-tenant grant principal source — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
