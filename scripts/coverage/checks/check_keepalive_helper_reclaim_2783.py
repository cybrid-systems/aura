#!/usr/bin/env python3
"""Issue #2783: keepalive helper exits on body hard-reclaim (no orphan leak).

JoinStatus::Reclaimed (#2227) left the keepalive helper parking forever
(helper_stop-only continue while body never sets body_done). Helper now
exits on body is_reclaimed(); join_keepalive_helper orphan-registers residual.

Contract (one row per AC):
  AC1 helper loop watches body is_reclaimed + reclaim_exit metric
  AC2 join_keepalive_helper orphan path (note_orphan + orphan_total)
  AC3 schema-2783 query keys + kKeepaliveHelperReclaimIssue
  AC4 ac2783_* in test_fiber_native_keepalive; no test_issue_2783.cpp
  AC5 this linter wired; no docs/design/2783-*

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

    spawn = _read("src/orch/agent_spawn.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_fiber_native_keepalive.cpp")
    build = _read("build.py")

    # AC1
    must("kKeepaliveHelperReclaimIssue", "AC1", spawn)
    must("2783", "AC1", spawn)
    must("is_reclaimed()", "AC1", spawn)
    must("keepalive_helper_reclaim_exit_total", "AC1", spawn)
    must("body_ptr", "AC1", spawn)

    # AC2
    must("keepalive_helper_orphan_total", "AC2", spawn)
    must("note_orphan_fiber", "AC2", spawn)
    must("request_force_safepoint", "AC2", spawn)

    # AC3
    must("schema-2783", "AC3", prim)
    must("keepalive-helper-orphan-total", "AC3", prim)
    must("keepalive-helper-reclaim-exit-total", "AC3", prim)
    must("keepalive-helper-reclaim-wired", "AC3", prim)

    # AC4
    must("ac2783_reclaimed_body_helper_joins", "AC4", test)
    must("2783", "AC4", test)
    if (ROOT / "tests" / "orch" / "test_issue_2783.cpp").is_file():
        fails.append("AC4: test_issue_2783.cpp present (forbidden per #81967)")

    # AC5
    must("check_keepalive_helper_reclaim_2783", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2783-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2783 keepalive helper reclaim exit — is_reclaimed + orphan path + schema-2783")
    return 0


if __name__ == "__main__":
    sys.exit(main())
