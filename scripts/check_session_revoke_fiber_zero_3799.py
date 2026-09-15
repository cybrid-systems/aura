#!/usr/bin/env python3
"""Issue #3799: session revoke must never mid-only (fiber_id=0) under
Restricted+MT (hard_fiber_isolation) — peer outermost collateral.

Contract (one row per AC):
  AC1  dual-fiber real fid: outermost/steal/join pass non-zero Fiber::id /
       captured fiber_id_ when fiber live; registry filters peer grants
  AC2  fiber_id=0 under Restricted+hard_fiber fail-closes (orphan observe,
       revoke nothing) — no silent mid-only sweep
  AC3  Soft/Off live==0 short-circuit + soft-share Restricted mid-only retained
  AC4  cite-first linter wired; tests extend test_capability_single_use_consume;
       no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    cap = _read("src/core/capability_model.hh")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    eval_ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    must("kCapabilitySessionRevokeFiberZeroIssue = 3799", "AC2 stamp", cap)
    must("Issue #3799", "AC2 cap cite", cap)
    must("fiber_id == 0 && production", "AC2 fail-closed gate", cap)
    must("hard_fiber_isolation_", "AC2 MT hard_fiber", cap)
    must("session_bound_orphan_detected_total", "AC2 orphan observe", cap)

    must("Issue #3799", "AC1 dtor cite", bound)
    must("fiber_id_", "AC1 captured fiber", bound)
    must("revoke_session_grants_for_mid_locked(session_mid_at_enter_", "AC1 dtor mid helper", bound)
    # Prefer captured fiber_id_ before aura_fiber_current_id in revoke window.
    dtor = bound.find("Issue #3799")
    if dtor < 0:
        fails.append("AC1: outermost #3799 cite missing")
    else:
        win = bound[dtor : dtor + 900]
        must("fiber_id_", "AC1 dtor uses fiber_id_", win)
        must("aura_fiber_current_id()", "AC1 dtor fallback current id", win)

    must("Issue #3799", "AC1 steal/join cite", steal)
    must("captured_fiber_id()", "AC1 Guard accessor use or decl", steal + eval_ixx)
    must("static_cast<std::uint32_t>(f->id())", "AC1 join Fiber::id", steal)
    must("static_cast<std::uint32_t>(fiber->id())", "AC1 steal Fiber::id", steal)

    must("zero extra work when no session grants", "AC3 Soft mid", cap)
    must("AC4: Soft / empty live residual — no lock", "AC3 Soft steal", cap)

    if "capability_peer_fiber_revoke_total" in cap or "session_fiber_zero_refuse_total" in cap:
        fails.append("AC4: new mid-struct counter (forbidden)")
    must("capability_session_revoke_steal_total", "AC4 reuse steal counter", cap)
    must("session_bound_orphan_detected_total", "AC4 reuse orphan counter", cap)

    must("ac3799_1_dual_fiber_real_fid_no_peer_collateral", "AC4 AC1 test", test)
    must("ac3799_2_fiber_zero_fail_closed_under_mt", "AC4 AC2 test", test)
    must("ac3799_3_soft_zero_cost_retained", "AC4 AC3 test", test)
    must("ac3799_4_source_cite_and_linter", "AC4 AC4 test", test)
    must("check_session_revoke_fiber_zero_3799", "AC4 build.py", build)
    if (ROOT / "tests" / "core" / "test_issue_3799.cpp").is_file():
        fails.append("AC4: tests/core/test_issue_3799.cpp present (forbidden #81967)")
    if _read("docs/design/3799-session-revoke-fiber-zero.md"):
        fails.append("AC4: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3799 session_revoke_fiber_zero:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3799 session_revoke_fiber_zero: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
