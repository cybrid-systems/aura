#!/usr/bin/env python3
"""Issue #3334: production typed abandon after Reclaimed Timeout.

Long-lived C++ hosts that keep AgentHandle after the 50 ms auto-wait
Timeout needed a first-class abandon path: bounded second wait, then
detach mailbox attach + release reservation + clear name, never free
body-stack while !is_done (#2661). Distinct from dtor under-account.

Contract:
  AC1  production + must_wait + live body + abandon → reservation/name gone,
       body-stack live, reclaimed_abandon_total bumps
  AC2  host never abandons → host_forget + dtor under-account unchanged
  AC3  Soft / Off: Invalid, zero extra wait / atomic
  AC4  body already done → Cleaned, not Abandoned
  AC5  extend test_join_drain_reclaim; linter after #3297; no invent / docs

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

    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_join_drain_reclaim.cpp")
    lint3297 = _read("scripts/coverage/checks/check_reclaimed_dtor_under_account_3297.py")
    build = _read("build.py")

    must("kReclaimedAbandonIssue = 3334", "AC1 stamp", spawn)
    must("reclaimed_abandon_total{0}", "AC1 counter at struct end", spawn)
    must("abandon_reclaimed", "AC1 helper", spawn)
    must("AbandonReclaimedOutcome", "AC1 outcome", spawn)
    must("ac3334_1_abandon_releases_without_body_stack", "AC1 test", test)
    dtor_pos = spawn.find("reclaimed_dtor_under_account_total{0}")
    ab_pos = spawn.find("reclaimed_abandon_total{0}")
    if ab_pos < 0 or dtor_pos < 0 or ab_pos < dtor_pos:
        fails.append("AC1: reclaimed_abandon_total must be AFTER dtor under-account (#2906)")

    must("host_forget_reclaimed_risk_total", "AC2 forget path", spawn)
    must("reclaimed_dtor_under_account_total", "AC2 dtor path", spawn)
    must("ac3334_2_forget_path_unchanged", "AC2 test", test)
    must("Issue #3297", "AC2 3297 linter retained", lint3297)

    must("if (!h.must_wait_reclaimed)", "AC3 Soft gate", spawn)
    must("ac3334_3_soft_zero_cost", "AC3 test", test)

    must("AbandonReclaimedOutcome::Cleaned", "AC4 Cleaned", spawn)
    must("ac3334_4_cleaned_when_body_done", "AC4 test", test)
    if "fiber->is_done()" not in spawn[spawn.find("abandon_reclaimed") :]:
        fails.append("AC1/AC4: abandon must consult fiber->is_done() (#2661)")

    must("reclaimed-abandon-total", "AC5 stats key", prim)
    must("schema-3334", "AC5 schema on existing query", prim)
    must("check_reclaimed_abandon_3334", "AC5 build.py", build)
    must("ac3334_5_source_and_linter", "AC5 test", test)
    prev = build.find("check_reclaimed_dtor_under_account_3297")
    ours = build.find("check_reclaimed_abandon_3334")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3297")
    if (ROOT / "tests" / "orch" / "test_issue_3334.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_3334.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3334-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3334 reclaimed abandon — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
