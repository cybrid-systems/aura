#!/usr/bin/env python3
"""Issue #2944: mutation-session grants (mid-bound + boundary exit revoke).

Contract (one row per AC):
  AC1  session_bound + grant_effect_session / grant_session + mid stamp
  AC2  outermost MutationBoundary exit calls revoke_session_grants_for_mid
  AC3  live_session_grants early-out (zero cost when empty)
  AC4  interop with single_use / high-risk force / durable path
  AC5  schema-2944 + session counters on posture
  AC6  source-cite + tests; no invent/design

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
    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    # AC1
    must("session_bound", "AC1", cap)
    must("grant_session", "AC1", cap)
    must("grant_effect_session", "AC1", sec)
    must("grant_effect_session", "AC1", ixx)
    must("Issue #2944", "AC1", cap)

    # AC2
    must("revoke_session_grants_for_mid", "AC2", cap)
    must("revoke_session_grants_for_mid", "AC2", bound)
    must("session_mid_at_enter_", "AC2", bound)
    must("session-mid-exit", "AC2", cap)
    must("is_outermost_", "AC2", bound)

    # AC3
    must("capability_live_session_grants", "AC3", cap)
    must("live_session_grants", "AC3", cap)
    if "capability_live_session_grants.load" not in cap and "live_session_grants" not in cap:
        fails.append("AC3: live residual early-out missing")

    # AC4
    must("single_use", "AC4", cap)
    must("high-risk", "AC4", sec.lower() + sec)  # force path in grant_effect_session
    must("grant_effect_session", "AC4", sec)
    if "single_use" not in sec or "kHighRiskMask" not in sec:
        fails.append("AC4: grant_effect_session must interop with high-risk force")

    # AC5
    must("schema-2944", "AC5", posture)
    must("mutation-session-grant-wired", "AC5", posture)
    must("capability-session-grant-total", "AC5", posture)
    must("capability-session-revoke-total", "AC5", posture)
    must("capability_session_revoke_total", "AC5", cap)
    must("capability_session_grant_total", "AC5", cap)

    # AC6
    must("2944", "AC6", test)
    must("grant_session", "AC6", test)
    must("revoke_session_grants_for_mid", "AC6", test)
    must("check_mutation_session_grant_2944", "AC6", build)
    if (ROOT / "tests" / "core" / "test_issue_2944.cpp").is_file():
        fails.append("AC6: test_issue_2944.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2944-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2944 mutation-session grants (mid-bound + boundary revoke)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
