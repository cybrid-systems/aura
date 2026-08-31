#!/usr/bin/env python3
"""Issue #3458: TenantScope::release revokes (scope tenant, mid at enter).

#3142 froze (prev_tenant_, live epoch) as AC1 — wrong tuple on both
axes. The scope principal's session grants stayed sticky past scope
exit and the previous principal's unrelated grants risked collateral
revoke at the live Mutation epoch.

Contract:
  AC1 ctor captures scope_tenant_ = tenant_id and
      scope_mid_ = fiber session mid ?: current Mutation epoch
  AC2 release revokes (scope_tenant_, scope_mid_, fiber_id_) under the
      same registry mtx; reason "scope-dtor-cascade" stays (no new
      query key); wrong tuple (prev_tenant_, live epoch) gone
  AC3 Soft/Off + capability_live_session_grants == 0: zero-cost
      short-circuit unchanged
  AC4 outermost mid-exit (#2944) + steal resume (#3048/#3320) callers
      of the same SSOT untouched
  AC5 tests/core/test_capability_single_use_consume.cpp extends
      ac3458_*; build.py wires the linter; no docs/design/, no
      tests/issues/

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

    sec = _read("src/compiler/evaluator_security.cpp")
    must("Issue #3458", "AC1 stamp", sec)
    must("scope_tenant_(tenant_id)", "AC1 ctor captures scope principal", sec)
    must(
        "scope_mid_(aura::serve::current_fiber_session_mid())",
        "AC1 ctor captures fiber session mid",
        sec,
    )
    must("scope_mid_ = ::aura::core::current_mutation_epoch()", "AC1 ctor epoch fallback", sec)

    rel = sec.find("void Evaluator::TenantScope::release() noexcept")
    win = sec[rel : rel + 4200] if rel >= 0 else ""
    must(
        "revoke_session_grants_for_locked(scope_tenant_, scope_mid_, fiber_id_",
        "AC2 revoke key",
        win,
    )
    if "revoke_session_grants_for_locked(prev_tenant_" in sec:
        fails.append("AC2: wrong tuple (prev_tenant_, live epoch) still present")
    must('"scope-dtor-cascade"', "AC2 reason string", win)
    must("capability_live_session_grants.load", "AC3 zero-cost short-circuit", win)
    must("scope_mid_ != 0 && (production || have_live)", "AC3 guard", win)

    ixx = _read("src/compiler/evaluator.ixx")
    must("std::uint64_t scope_tenant_ = 0;", "AC1 member scope_tenant_", ixx)
    must("std::uint64_t scope_mid_ = 0;", "AC1 member scope_mid_", ixx)
    must("scope_tenant_(o.scope_tenant_)", "AC1 move copies scope_tenant_", ixx)
    must("scope_mid_(o.scope_mid_)", "AC1 move copies scope_mid_", ixx)

    boundary = _read("src/compiler/evaluator_mutation_boundary.cpp")
    must("revoke_session_grants_for_mid_locked", "AC4 outermost mid-exit caller", boundary)
    fiber_mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    must("revoke_session_grants_on_steal_or_abort_locked", "AC4 steal resume caller", fiber_mut)

    test = _read("tests/core/test_capability_single_use_consume.cpp")
    must("ac3458_1_scope_tenant_revoked_prev_untouched", "AC5 AC1 test", test)
    must("ac3458_2_epoch_bump_keys_enter_mid", "AC5 AC2 test", test)
    must("ac3458_3_source_cite_ssot_callers_intact", "AC5 source-cite test", test)

    build = _read("build.py")
    must("check_tenant_scope_release_key_3458", "AC5 build.py wires linter", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3458-*")):
            fails.append(f"AC5: docs/design/{f.name}")
    if (ROOT / "tests" / "issues" / "test_issue_3458.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3458.cpp")

    if fails:
        print("FAIL #3458 tenant_scope_release_key:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3458 tenant_scope_release_key: (scope tenant, mid at enter) revoked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
