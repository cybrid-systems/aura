#!/usr/bin/env python3
"""Issue #3209: session_mid × nested abort × steal residual linearization.

#3142 / #3048 closed the primary steal / nested TenantScope abort
revoke paths. This issue is the composed happens-before:
mark_stolen → revoke_for_mid → clear mid (commutative no-op pair).

Contract:
  AC1 after outermost exit / nested abort / steal / TenantScope dtor:
      session_bound_entries_alive==0 (or remaining stolen + consume deny)
  AC2 no double-consume of single_use+session_bound after steal mark
  AC3 Soft/Off zero-cost unchanged
  AC4 existing metrics only
  AC5 Restricted matrix test; no invent

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

    cap = _read("src/core/capability_model.hh")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fiber_h = _read("src/serve/fiber.h")
    fiber_c = _read("src/serve/fiber.cpp")
    test = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    must("kCapabilitySessionQuiesceIssue = 3209", "AC5 stamp", cap)
    must("mark_session_bound_stolen_for_mid_locked", "AC1 for-mid stolen", cap)
    must("mark_stolen before revoke", "AC1 happens-before", cap)

    must("Issue #3209", "AC1 steal cite", steal)
    must("aura_fiber_clear_session_mid", "AC1 victim mid clear", steal)
    must("clear_session_mid()", "AC1 steal_complete victim clear", steal)

    must("Issue #3209", "AC1 Guard dtor cite", bound)
    must("revoke_session_grants_for_mid_locked", "AC1 dtor locked revoke", bound)
    must("clear_current_fiber_session_mid", "AC1 dtor clear mid", bound)

    must("aura_fiber_clear_session_mid", "AC1 fiber.h", fiber_h)
    must("aura_fiber_clear_session_mid", "AC1 fiber.cpp", fiber_c)

    # AC3 Soft short-circuit still present
    must("AC4: Soft / empty live residual — no lock", "AC3 Soft", cap)

    # AC4 no new counters
    if "session_quiesce_total" in cap or "capability_steal_resume_total" in cap:
        fails.append("AC4: new mid-struct counter (forbidden)")
    must("session_bound_revoked_on_steal_total", "AC4 reuse steal counter", cap)
    must("capability_session_revoke_steal_total", "AC4 reuse session_revoke_steal", cap)
    must("session_bound_revoked_on_scope_dtor_total", "AC4 reuse dtor counter", cap)

    must("ac3209_1_outermost_exit_success_and_fail", "AC5 matrix exit", test)
    must("ac3209_2_nested_abort_then_outermost", "AC5 nested abort", test)
    must("ac3209_3_steal_no_double_consume", "AC5 steal consume", test)
    must("ac3209_4_composed_chaos", "AC5 composed chaos", test)
    must("ac3209_5_soft_zero_cost_and_source", "AC5 Soft + linter", test)
    must("single-use-consumed", "AC2 stolen consume reason", test)
    must("check_session_grant_quiesce_3209", "AC5 build.py", build)

    if (ROOT / "tests" / "core" / "test_issue_3209.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3209.cpp")
    if (ROOT / "tests" / "core" / "test_session_bound_nested_abort_steal.cpp").is_file():
        fails.append("AC5: dedicated invent test file")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3209-*")):
            fails.append(f"AC5: docs/design/{f.name}")
    if "class AgentRegistry" in cap:
        fails.append("AC5: AgentRegistry")

    if fails:
        print("FAIL: Issue #3209 linter found", len(fails), "problems:")
        for f in fails:
            print(" -", f)
        return 1
    print("OK: Issue #3209 — session grant steal × nested abort × resume quiesce.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
