#!/usr/bin/env python3
"""Issue #2967: durable high-risk grant call-site gate (TenantAdmin + reason).

Contract (one row per AC):
  AC1  Under production (Restricted/Strict: sandbox_mode_ != 0 ||
       effect_sandbox_mode() != 0), grant_effect_durable with any high-risk
       effect bit (Mutate | MacroSelfEvo | TenantAdmin | Syscall) requires
       the caller to hold TenantAdmin (or the "tenant-admin" / "capability"
       string caps that map to it via effect_for_cap_name). Missing → deny +
       SE EffectDeny reason 'durable-grant-needs-tenant-admin' + deny counter.
  AC2  Durable high-risk grants must stamp a non-empty agent-stable reason
       into the audit; empty reason under production → deny + SE reason
       'durable-grant-reason-required'.
  AC3  Soft / Off path (sandbox_mode_ == 0 && effect_sandbox_mode() == 0)
       short-circuits: zero added cost, no privilege lookup, no deny.
  AC4  Allow counter (capability_durable_high_risk_grant_total) bumps ONLY
       on allow; #2882 forced-single-use surface unchanged.
  AC5  Additive query keys only (schema-2967): capability-durable-grant-
       deny-total + durable-grant-tenant-admin-wired + durable-grant-reason-
       wired; snapshot exposes capability_durable_grant_deny.
  AC6  Source-cite in evaluator_security.cpp + evaluator.ixx +
       capability_model.hh; tests extend existing capability single-use
       suite per #81967 (no new test_issue_NNNN.cpp); no docs/design per
       #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SCOPE_FILES = [
    "src/core/capability_model.hh",
    "src/compiler/evaluator_security.cpp",
    "src/compiler/evaluator.ixx",
    "src/compiler/evaluator_primitives_security.cpp",
    "tests/core/test_capability_single_use_consume.cpp",
    "scripts/coverage/checks/check_capability_durable_gate_2967.py",
]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    cap_model = _read("src/core/capability_model.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    sec_caps = _read("src/compiler/security_capabilities.h")
    test_su = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    # ── AC1: TenantAdmin gate under production ────────────────────
    must("Issue #2967", "AC1", sec)
    must("durable-grant-needs-tenant-admin", "AC1", sec)
    must("capability_durable_grant_deny_total", "AC1", cap_model)
    must("capability_durable_grant_deny_total", "AC1", sec)
    # Gate must key off production (sandbox/effect mode), matching #2882
    # force_bind semantics (AC3 zero-cost when both off).
    must("force_bind", "AC1", sec)
    must("has_capability(kCapTenantAdmin)", "AC1", sec)
    # TenantAdmin cap name + capability alias must exist.
    must("kCapTenantAdmin", "AC1", sec_caps)
    must("kCapCapability", "AC1", sec_caps)
    # The deny must precede the allow counter bump (use fetch_add sites, not
    # comment mentions — comments mention both counters before the gate).
    deny_p = sec.find("capability_durable_grant_deny_total.fetch_add")
    allow_p = sec.find("capability_durable_high_risk_grant_total.fetch_add")
    if deny_p < 0 or allow_p < 0 or deny_p > allow_p:
        fails.append("AC1: deny counter fetch_add must precede allow counter fetch_add in grant_effect_durable")

    # ── AC2: mandatory reason under production ────────────────────
    must("durable-grant-reason-required", "AC2", sec)
    must("reason.empty()", "AC2", sec)
    # Signature carries the reason parameter (default empty).
    must("std::string_view reason = {}", "AC2", ixx)

    # ── AC3: Soft/Off zero-cost short-circuit ─────────────────────
    must("force_bind && is_high_risk", "AC3", sec)

    # ── AC4: allow counter only on allow; #2882 surface preserved ─
    must("capability_durable_high_risk_grant_total", "AC4", sec)
    must("capability_high_risk_forced_single_use_total", "AC4", cap_model)
    must("capability_high_risk_forced_single_use_total", "AC4", sec)

    # ── AC5: additive query keys + snapshot ───────────────────────
    must("schema-2967", "AC5", posture)
    must("issue-2967", "AC5", posture)
    must("capability-durable-grant-deny-total", "AC5", posture)
    must("durable-grant-tenant-admin-wired", "AC5", posture)
    must("durable-grant-reason-wired", "AC5", posture)
    must("capability_durable_grant_deny", "AC5", cap_model)  # snapshot field

    # ── AC6: source-cite + tests + no invent + no docs/design/ ───
    must("#2967", "AC6", test_su)
    must("#2967", "AC6", ixx)
    must("#2967", "AC6", cap_model)
    must("#2967", "AC6", sec)
    must("check_capability_durable_gate_2967", "AC6", build)  # linter wired
    # The test file's own forbidden-check (no test_issue_2967.cpp) is allowed;
    # only a real new test file at either canonical path fails AC6.
    for rel in ("tests/core/test_issue_2967.cpp", "tests/compiler/test_issue_2967.cpp"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #81967")
    for rel in ("docs/design/2967-durable-gate.md", "docs/design/2967-*.md"):
        if _read(rel):
            fails.append(f"AC6: {rel} exists — forbidden per #1655")

    if fails:
        print(f"Issue #2967 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #2967 durable grant gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
