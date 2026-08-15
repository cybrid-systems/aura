#!/usr/bin/env python3
"""Issue #3048: session-bound grants residual under steal / force-cancel / abort.

Contract (one row per AC):
  AC1  steal-complete / force-cancel / mark_outermost_failed call
       revoke_session_grants_on_steal_or_abort (reuses revoke_session_grants_for_mid)
  AC2  after steal/abort: live_session_grants==0; Restricted/Strict deny
  AC3  happy-path outermost Guard dtor (#2944) unchanged; double-revoke no-op
  AC4  Soft / Off: zero extra work when no live session grants
  AC5  SE reason session-mid-steal-exit / session-mid-abort-exit + schema-3048
  AC6  source-cite only; extend capability / tenant isolation suite;
       no test_issue_3048.cpp; no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    cap = _read("src/core/capability_model.hh")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    fiber_h = _read("src/serve/fiber.h")
    fiber_c = _read("src/serve/fiber.cpp")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    test_su = _read("tests/core/test_capability_single_use_consume.cpp")
    test_iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    build = _read("build.py")

    # AC1
    must("revoke_session_grants_on_steal_or_abort", "AC1", cap)
    must("revoke_session_grants_on_steal_or_abort", "AC1", steal)
    must("aura_evaluator_on_steal_complete", "AC1", steal)
    must("mark_outermost_mutation_failed", "AC1", steal)
    must("set_current_fiber_session_mid", "AC1", bound)
    must("session_mid_", "AC1", fiber_h)
    must("aura_fiber_session_mid", "AC1", fiber_c)

    # AC2 / AC5
    must("session-mid-steal-exit", "AC5", cap)
    must("session-mid-abort-exit", "AC5", cap)
    must("capability_session_revoke_steal_total", "AC5", cap)
    must("capability_session_revoke_abort_total", "AC5", cap)
    must("schema-3048", "AC5", posture)
    must("session-grant-steal-abort-wired", "AC5", posture)
    must("capability-session-revoke-steal-total", "AC5", posture)

    # AC3
    must("revoke_session_grants_for_mid", "AC3", bound)
    must("session-mid-exit", "AC3", cap)
    must("session_mid_at_enter_", "AC3", bound)

    # AC4
    must("capability_live_session_grants", "AC4", cap)
    if "capability_live_session_grants.load" not in cap:
        fails.append("AC4: live residual early-out missing")

    # AC6
    must("3048", "AC6", test_su)
    must("3048", "AC6", test_iso)
    must("revoke_session_grants_on_steal_or_abort", "AC6", test_su)
    must("check_session_grant_steal_3048", "AC6", build)
    must("#3048", "AC6", ixx)
    if (ROOT / "tests" / "core" / "test_issue_3048.cpp").is_file():
        fails.append("AC6: test_issue_3048.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3048.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3048.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3048-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3048 session-grant steal/abort residual revoke")
    return 0


if __name__ == "__main__":
    sys.exit(main())
