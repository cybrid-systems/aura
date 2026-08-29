#!/usr/bin/env python3
"""Issue #3126: TOCTOU in TenantAdmin check vs grant (unlocked effects_for).

CapabilityRegistry::effects_for / provenance_ok read `by_tenant` without
holding the registry mutex; the security fence in
Evaluator::grant_effect_* / revoke_effect_capability calls these (via
has_capability) between the admin check and the act, creating a
classic TOCTOU window: a momentarily-held TenantAdmin can land a
foreign-tenant grant before revocation by a concurrent Evaluator /
fiber-steal activity.

Fix:
  AC1 capability_model.hh: add effects_for_locked + provenance_ok_locked +
      grant_locked + revoke_locked. Mark public effects_for / provenance_ok
      "Soft/observational only".
  AC2 evaluator_security.cpp grant_effect_capability: foreign-tenant
      fence takes registry mtx + uses effects_for_locked + grant_locked.
  AC3 evaluator_security.cpp grant_effect_durable: foreign-tenant
      fence + high-risk TenantAdmin+reason gate share one locked
      is_admin via effects_for_locked under mtx + grant_locked.
  AC4 evaluator_security.cpp grant_effect_session: foreign-tenant
      fence takes mtx + effects_for_locked + grant_locked.
  AC5 evaluator_security.cpp revoke_effect_capability: foreign-tenant
      fence takes mtx + effects_for_locked + revoke_locked.
  AC6 tests/core/test_tenant_isolation_enforcement.cpp: ac3126
      source-cite + Soft consistency. Existing #2490 / #2529 test files
      stay green (no deletion / no removed ACs).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    cm = _read("src/core/capability_model.hh")
    es = _read("src/compiler/evaluator_security.cpp")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    test_req = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    test_retain = _read("tests/compiler/test_grant_epoch_retain_restricted.cpp")

    # AC1 — capability_model.hh locked variants + Soft/observational comments.
    must("effects_for_locked", "AC1", cm)
    must("provenance_ok_locked", "AC1", cm)
    must("grant_locked", "AC1", cm)
    must("revoke_locked", "AC1", cm)
    must("Soft/observational only", "AC1", cm)
    must("DO NOT use for security decisions under concurrent mutation", "AC1", cm)
    must("Issue #3126", "AC1", cm)
    # Public unlocked effects_for / provenance_ok still exist (no API break).
    must("[[nodiscard]] Effect effects_for(TenantId tenant) const", "AC1", cm)
    must("[[nodiscard]] bool provenance_ok(TenantId tenant, const EffectProvenance& prov", "AC1", cm)

    # AC2 — grant_effect_capability fence locks + uses locked variants.
    cap_pos = es.find("void Evaluator::grant_effect_capability(")
    durable_pos = es.find("void Evaluator::grant_effect_durable(", cap_pos)
    cap_block = es[cap_pos:durable_pos] if durable_pos > cap_pos else es[cap_pos:]
    must("std::lock_guard<std::mutex> lock(reg.mtx)", "AC2", cap_block)
    must("effects_for_locked(self_tenant)", "AC2", cap_block)
    must("has_effect(held, Effect::TenantAdmin)", "AC2", cap_block)
    must("reg.grant_locked(", "AC2", cap_block)
    # Legacy unlocked admin fence removed.
    if "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)" in cap_block:
        fails.append("AC2: grant_effect_capability still has unlocked has_capability fence")

    # AC3 — grant_effect_durable: foreign-tenant + high-risk share locked is_admin.
    sess_pos = es.find("void Evaluator::grant_effect_session(", durable_pos)
    dur_block = es[durable_pos:sess_pos] if sess_pos > durable_pos else es[durable_pos:]
    must("std::lock_guard<std::mutex> lock(reg_durable.mtx)", "AC3", dur_block)
    must("effects_for_locked(self_tenant)", "AC3", dur_block)
    must("has_effect(held_durable, Effect::TenantAdmin)", "AC3", dur_block)
    must("reg_durable.grant_locked(", "AC3", dur_block)
    if "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)" in dur_block:
        fails.append("AC3: grant_effect_durable still has unlocked has_capability fence")

    # AC4 — grant_effect_session fence locks + uses locked variants.
    rev_pos = es.find("void Evaluator::revoke_effect_capability(", sess_pos)
    sess_block = es[sess_pos:rev_pos] if rev_pos > sess_pos else es[sess_pos:]
    must("std::lock_guard<std::mutex> lock(reg_session.mtx)", "AC4", sess_block)
    must("effects_for_locked(self_tenant)", "AC4", sess_block)
    must("reg_session.grant_locked(", "AC4", sess_block)
    if "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)" in sess_block:
        fails.append("AC4: grant_effect_session still has unlocked has_capability fence")

    # AC5 — revoke_effect_capability fence locks + uses revoke_locked.
    # Bound to the next Evaluator method so later sites (e.g. #3411
    # set_tenant_principal) are not scored as the revoke fence.
    sandbox_pos = es.find("void Evaluator::set_effect_sandbox_mode(", rev_pos)
    rev_block = es[rev_pos:sandbox_pos] if sandbox_pos > rev_pos else es[rev_pos:]
    must("std::lock_guard<std::mutex> lock(reg_revoke.mtx)", "AC5", rev_block)
    must("effects_for_locked(self_tenant)", "AC5", rev_block)
    must("reg_revoke.revoke_locked(", "AC5", rev_block)
    if "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)" in rev_block:
        fails.append("AC5: revoke_effect_capability still has unlocked has_capability fence")

    # AC6 — tests extension + existing #2490 / #2529 surface preserved.
    must("ac3126_admin_fence_locked", "AC6", test_iso)
    must("Issue #3126", "AC6 test source-cite", test_iso)
    must("effects_for_locked", "AC6 test cite", test_iso)
    must("grant_locked", "AC6 test cite", test_iso)
    must("Issue #2490", "AC6 #2490 still green", test_req)
    must("Auto-enforce workspace isolation", "AC6 #2490 surface intact", test_req)
    must("Issue #2529", "AC6 #2529 still green", test_retain)
    must("kDefaultGrantEpochRetainWindowRestricted", "AC6 #2529 surface intact", test_retain)
    # No new tests/issues/test_issue_3126.cpp per #81967 src/-aligned suite.
    issue_test = _read("tests/issues/test_issue_3126.cpp")
    if issue_test:
        fails.append("AC6: tests/issues/test_issue_3126.cpp exists (must NOT \u2014 src/-aligned only)")

    if fails:
        print("check_capability_admin_fence_3126: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_capability_admin_fence_3126: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
