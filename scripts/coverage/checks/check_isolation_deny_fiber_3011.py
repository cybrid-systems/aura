#!/usr/bin/env python3
"""Issue #3011: IsolationDeny SecurityEvent stamps live fiber id.

Contract (one row per AC):
  AC1  WorkspaceIsolationPolicy::record_audit IsolationDeny emit uses
       effect_fiber_id_or(aura_fiber_current_id()) — not hard-coded 0.
       Private IsolationAuditEntry.fiber_id stamped on deny only.
  AC2  EffectDeny path unchanged (capability_model record_audit still
       stamps prov.fiber_id).
  AC3  Soft / Off allow: fiber lookup only after denied==true so
       record_audit allow returns before TLS / override load.
  AC4  query:security-audit still filters by fiber (filt_fiber);
       query:security-stats + query:security-audit-stats expose
       schema-3011 / isolation-deny-fiber-wired.
  AC5  Extend test_tenant_isolation_enforcement (#81967); no
       test_issue_3011.cpp; no docs/design/ (#1655).

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
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # ── AC1: IsolationDeny emit stamps live / override fiber ───────
    must("Issue #3011", "AC1", iso)
    must("effect_fiber_id_or", "AC1", iso)
    must("aura_fiber_current_id", "AC1", iso)
    must("IsolationDeny", "AC1", iso)
    if "/*fiber_id=*/0" in iso:
        fails.append("AC1: record_audit still hard-codes /*fiber_id=*/0")
    emit_p = iso.find("emit_security_event_durable(SecurityEventKind::IsolationDeny")
    if emit_p < 0:
        fails.append("AC1: IsolationDeny emit_security_event_durable not found")
    else:
        emit_chunk = iso[emit_p : emit_p + 400]
        if "entry.fiber_id" not in emit_chunk and "fid" not in emit_chunk:
            fails.append("AC1: IsolationDeny emit must pass resolved fiber id")

    # ── AC2: EffectDeny unchanged ─────────────────────────────────
    must("static_cast<std::int64_t>(prov.fiber_id)", "AC2", cap)
    must("EffectDeny", "AC2", cap)

    # ── AC3: fiber lookup only on deny ────────────────────────────
    rec = iso.find("void record_audit(")
    body = iso[rec:] if rec >= 0 else ""
    deny_lookup = body.find("if (denied)")
    fiber_or = body.find("effect_fiber_id_or")
    allow_ret = body.find("if (!denied)")
    if rec < 0 or deny_lookup < 0 or fiber_or < 0 or allow_ret < 0 or not (deny_lookup < fiber_or < allow_ret):
        fails.append("AC3: effect_fiber_id_or must sit in denied-only block before allow return")

    # ── AC4: query filter + additive keys ─────────────────────────
    must("filt_fiber", "AC4", posture)
    must("schema-3011", "AC4", posture)
    must("issue-3011", "AC4", posture)
    must("isolation-deny-fiber-wired", "AC4", posture)
    must("query:security-stats", "AC4", posture)
    must("query:security-audit", "AC4", posture)

    # ── AC5: tests + no invent + no docs/design/ ──────────────────
    must("#3011", "AC5", test_iso)
    must("fiber_id == 42", "AC5", test_iso)
    must("check_isolation_deny_fiber_3011", "AC5", build)
    must("#3011", "AC5", iso)
    for rel in ("tests/core/test_issue_3011.cpp", "tests/compiler/test_issue_3011.cpp"):
        if _read(rel):
            fails.append(f"AC5: {rel} exists — forbidden per #81967")
    for rel in (
        "docs/design/3011-isolation-deny-fiber.md",
        "docs/design/3011-*.md",
    ):
        if _read(rel):
            fails.append(f"AC5: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #3011 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3011 IsolationDeny fiber stamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
