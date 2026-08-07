#!/usr/bin/env python3
"""Issue #2705: production hard-close FlatAST global capture fallback.

Refines #2687 residual: maybe_stamp_stable_ref_isolation_tenant must fail
closed under production multi-tenant / Strict / AURA_HARD_CAPTURE_TENANT
instead of stamping from process-global g_isolation_capture_tenant.

Contract:
  AC1 Production hard-close armed → maybe_stamp refuses stamp when global
      tenant != 0; bumps evaluator_miss; global_fallback stays 0.
  AC2 Soft / tenant=0 / AURA_SANDBOX=off → zero-cost permissive (tid=0
      early return; hard-close pref cleared under sandbox=off).
  AC3 Evaluator::stamp_stable_ref / make_stamped_ref / export_ref still
      bump local only; set_tenant_principal must not write global
      (#2659 / #2687 regression).
  AC4 resolve_stamped + require_effect_on_ref foreign-ref deny still present
      (#2658 / #2689 lineage).
  AC5 Additive query: isolation-capture-hard-close-armed + schema-2705 /
      issue-2705; #2687 keys preserved.
  AC6 Source-cite + coverage linter; extend test_tenant_isolation_enforcement;
      no docs/design/* per #1655.

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
    sec_def = _read("src/compiler/security_defaults.hh")
    eval_sec = _read("src/compiler/evaluator_security.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # AC1 — hard-close armed + maybe_stamp refuses + evaluator_miss bump.
    must("kHardCaptureTenantIssue", "AC1", prov)
    must("g_hard_capture_tenant_pref", "AC1", prov)
    must("set_hard_capture_tenant", "AC1", prov)
    must("hard_capture_tenant_active", "AC1", prov)
    must("AURA_HARD_CAPTURE_TENANT", "AC1", prov)
    must("#2705", "AC1", prov)

    # maybe_stamp body must hard-close + bump miss (not global_fallback only).
    # Brace-count extract — nested if(){} defeats non-greedy regex.
    def _fn_body(hay: str, sig: str) -> str:
        i = hay.find(sig)
        if i < 0:
            return ""
        brace = hay.find("{", i)
        if brace < 0:
            return ""
        depth = 0
        for j in range(brace, len(hay)):
            if hay[j] == "{":
                depth += 1
            elif hay[j] == "}":
                depth -= 1
                if depth == 0:
                    return hay[brace + 1 : j]
        return ""

    # Prefer the function signature, not comment mentions of the name.
    body = _fn_body(prov, "bool maybe_stamp_stable_ref_isolation_tenant")
    if not body:
        body = _fn_body(prov, "maybe_stamp_stable_ref_isolation_tenant")
    if not body:
        fails.append("AC1: maybe_stamp_stable_ref_isolation_tenant impl not found")
    else:
        if "hard_capture_tenant_active" not in body:
            fails.append("AC1: maybe_stamp must consult hard_capture_tenant_active")
        if "g_isolation_capture_stamp_evaluator_miss_total_atomic" not in body:
            fails.append("AC1: maybe_stamp hard-close must bump evaluator_miss")
        if "g_isolation_capture_stamp_global_fallback_total_atomic" not in body:
            fails.append("AC1: Soft path must still bump global_fallback (legacy #2687)")
        hard_idx = body.find("hard_capture_tenant_active")
        miss_idx = body.find("g_isolation_capture_stamp_evaluator_miss_total_atomic")
        fallback_idx = body.find("g_isolation_capture_stamp_global_fallback_total_atomic")
        if hard_idx < 0 or miss_idx < 0 or fallback_idx < 0:
            fails.append("AC1: hard/miss/fallback markers incomplete in maybe_stamp")
        elif not (hard_idx < miss_idx < fallback_idx):
            fails.append("AC1: hard-close + miss bump must precede global_fallback soft path")

    # Production arming in apply_production_security_defaults.
    must("set_hard_capture_tenant", "AC1", sec_def)
    must("AURA_HARD_CAPTURE_TENANT", "AC1", sec_def)
    must("#2705", "AC1", sec_def)
    must("multi_tenant", "AC1", sec_def)

    # AC2 — Soft / tenant=0 early return preserved.
    must("if (tid == 0)", "AC2", prov)
    if body and "return false" not in body:
        fails.append("AC2: maybe_stamp must return false on tid==0 / hard-close")
    # sandbox=off clears hard pref.
    if not re.search(r"set_hard_capture_tenant\s*\(\s*false\s*\)", sec_def):
        fails.append("AC2: sandbox=off must clear hard_capture via set_hard_capture_tenant(false)")

    # AC3 — Evaluator local path + set_tenant_principal no global write.
    must("g_isolation_capture_stamp_local_total_atomic", "AC3", eval_sec)
    must("capability_tenant_id_", "AC3", eval_sec)
    m_st = re.search(
        r"void\s+Evaluator::set_tenant_principal\s*\([^)]*\)\s*(?:noexcept)?\s*\{(.+?)\n\s*\}",
        eval_sec,
        re.MULTILINE | re.DOTALL,
    )
    if not m_st:
        fails.append("AC3: Evaluator::set_tenant_principal impl not found")
    elif "set_isolation_capture_tenant" in m_st.group(1):
        fails.append("AC3 (#2659/#2687 regression): set_tenant_principal must NOT write global")

    # AC4 — require_effect / resolve lineage still present.
    must("require_effect_on_ref", "AC4", eval_sec)
    must("#2689", "AC4", eval_sec)
    must("#2658", "AC4", eval_sec + prov)

    # AC5 — additive query surface; #2687 preserved.
    must("isolation-capture-hard-close-armed", "AC5", q)
    must("schema-2705", "AC5", q)
    must("issue-2705", "AC5", q)
    must("hard_capture_tenant_active", "AC5", q)
    must("schema-2687", "AC5", q)
    must("issue-2687", "AC5", q)
    must("isolation-capture-stamp-local-total", "AC5", q)
    must("isolation-capture-stamp-global-fallback-total", "AC5", q)
    must("isolation-capture-stamp-evaluator-miss-total", "AC5", q)

    # AC6 — source-cite + no design docs + test extend + gate wire.
    must("#2705", "AC6", prov)
    must("#2705", "AC6", sec_def)
    must("#2705", "AC6", q)
    must("#2705", "AC6", test)
    must("hard_capture_tenant", "AC6", test)
    must("check_hard_capture_tenant_2705", "AC6", build)
    for rel in (
        "docs/design/hard_capture_tenant_2705.md",
        "docs/hard_capture_tenant_2705.md",
        "design/2705.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    linter_path = ROOT / "scripts/coverage/checks/check_hard_capture_tenant_2705.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2705 production hard-close FlatAST global capture — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
