#!/usr/bin/env python3
"""Issue #3241: concurrent outermost sharing epoch mid must not over-revoke
peer fiber session grants.

Contract (one row per AC):
  AC1  revoke_session_grants_for_mid_locked filters grant_fiber_id when
       fiber_id != 0; outermost dtor passes aura_fiber_current_id()
  AC2  steal/abort locked helper forwards fiber_id into mid-revoke
  AC3  TenantScope cascade remains fiber-aware (#3142)
  AC4  Soft/Off live==0 short-circuit unchanged
  AC5  existing SE reasons; no new metrics
  AC6  tests extend test_capability_single_use_consume; linter wired;
       no invent / docs/design

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
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    eval_sec = _read("src/compiler/evaluator_security.cpp")
    test = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    must("kCapabilitySessionPeerFiberIssue = 3241", "AC1 stamp", cap)
    must("Issue #3241", "AC1 cap cite", cap)
    must("fiber_id != 0 && g.grant_fiber_id != 0 && g.grant_fiber_id != fiber_id", "AC1 mid fiber filter", cap)
    must("Issue #3241", "AC1 dtor cite", bound)
    must("aura_fiber_current_id()", "AC1 dtor fiber", bound)
    must("revoke_session_grants_for_mid_locked(session_mid_at_enter_", "AC1 dtor mid helper", bound)

    must("revoke_session_grants_for_mid_locked(session_mid, reason, fiber_id)", "AC2 steal forwards fiber", cap)
    must("revoke_session_grants_on_steal_or_abort_locked", "AC2 steal locked", steal)

    must("revoke_session_grants_for_locked", "AC3 TenantScope", eval_sec)
    must(
        "fiber_id != 0 && g.grant_fiber_id != 0 && g.grant_fiber_id != fiber_id",
        "AC3 TenantScope filter still present",
        cap,
    )

    must("AC4: Soft / empty live residual — no lock", "AC4 Soft steal", cap)
    must("zero extra work when no session grants", "AC4 Soft mid", cap)

    must("session-mid-exit", "AC5 exit reason", cap)
    must("session-mid-steal-exit", "AC5 steal reason", cap)
    must("session-mid-abort-exit", "AC5 abort reason", cap)
    if "capability_peer_fiber_revoke_total" in cap or "session_peer_over_revoke" in cap:
        fails.append("AC5: new mid-struct counter (forbidden)")
    must("capability_session_revoke_total", "AC5 reuse session_revoke", cap)
    must("capability_session_revoke_steal_total", "AC5 reuse steal counter", cap)

    must("ac3241_1_peer_fiber_not_over_revoked", "AC6 AC1 test", test)
    must("ac3241_2_steal_a_does_not_touch_b", "AC6 AC2 test", test)
    must("ac3241_3_soft_zero_and_legacy_mid_only", "AC6 AC3 test", test)
    must("ac3241_4_source_cite_and_linter", "AC6 AC4 test", test)
    must("check_session_grant_peer_fiber_3241", "AC6 build.py", build)
    if (ROOT / "tests" / "core" / "test_issue_3241.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3241.cpp present (forbidden #81967)")
    if _read("docs/design/3241-session-grant-peer-fiber.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3241 session_grant_peer_fiber:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3241 session_grant_peer_fiber: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
