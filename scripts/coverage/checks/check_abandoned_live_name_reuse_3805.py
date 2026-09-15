#!/usr/bin/env python3
"""Issue #3805: abandon/force-recycle live-body husk must not be
move-assigned over by same-name put; name-table retires the map key
(erase + fresh insert). Directory / scope-resolve skip abandoned ghosts
as live send/join targets. Soft/Off + Done-path unchanged; #2661 preserved.

Contract:
  AC1  put/find retire abandoned-live (slot_is_abandoned_live) — never
       move-assign over a live fiber
  AC2  AgentScope find + directory_snapshot filter abandoned-live
  AC3  Soft #3467 deny + #3598 Done-path retire unchanged
  AC4  #2661 — no body-stack free while !is_done (predicate / recycle)
  AC5  suite rows in test_join_drain_reclaim.cpp; linter wired; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SPAWN = "src/orch/agent_spawn.h"
NAME_TABLE = "src/compiler/agent_name_table.h"
SCOPE = "src/orch/agent_scope.h"
PRIM = "src/compiler/evaluator_primitives_agent.cpp"
TEST = "tests/orch/test_join_drain_reclaim.cpp"
BUILD = "build.py"
LINTER = "check_abandoned_live_name_reuse_3805"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    spawn = _read(SPAWN)
    name_table = _read(NAME_TABLE)
    scope = _read(SCOPE)
    prim = _read(PRIM)
    test = _read(TEST)
    build = _read(BUILD)

    # AC1 — predicate + name-table retire (not move-assign over live).
    must("kAbandonedLiveNameReuseIssue = 3805", "AC1 stamp", spawn)
    must("slot_is_abandoned_live", "AC1 predicate", spawn)
    must("Issue #3805", "AC1 cite", spawn)
    must("h.name.empty() && !h.mailbox", "AC1 abandon-shape markers", spawn)
    must("slot_is_abandoned_live(it->second)", "AC1 name-table put/find", name_table)
    # Retire path must erase before emplace (shared with #3598).
    must("impl_->agents_.erase(it);", "AC1 erase-before-insert", name_table)
    must(
        "aura::orch::slot_is_reclaimable_clean(it->second) ||\n"
        "                aura::orch::slot_is_abandoned_live(it->second)",
        "AC1 put retire gate",
        name_table,
    )

    # AC2 — scope find + directory filter-out.
    must("slot_is_abandoned_live(h)", "AC2 scope find/directory", scope)
    must("filter-out; identity plane consistent", "AC2 directory filter cite", scope)

    # AC3 — Soft deny + Done-path still present.
    must(
        "if (it->second.must_wait_reclaimed || it->second.reclaimed_deferred_cleanup)",
        "AC3 #3467 deny retained",
        name_table,
    )
    must("slot_is_reclaimable_clean", "AC3 #3598 clean path retained", name_table)

    # AC4 — #2661 no body-stack free in recycle live arm.
    must_not("delete h.fiber", "AC4 no body-stack free", spawn)
    must("#2661", "AC4 cite in predicate/recycle", spawn)

    # AC5 — suite + build + no invent.
    must("ac3805_1_put_retires_abandoned_live_not_move_assign", "AC5 test AC1", test)
    must("ac3805_2_directory_scope_skip_abandoned_ghost", "AC5 test AC2", test)
    must("ac3805_3_soft_and_done_path_unchanged", "AC5 test AC3", test)
    must("ac3805_4_no_body_stack_free_on_retire", "AC5 test AC4", test)
    must("ac3805_5_source_cite_and_linter", "AC5 test AC5", test)
    must(LINTER, "AC5 build registration", build)
    must_not("abandoned-live-name-reuse", "AC5 no new query key", prim)
    for rel in ("tests/orch/test_issue_3805.cpp", "tests/issues/test_issue_3805.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: {rel} exists")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3805*"):
            fails.append(f"AC5: docs/design/{p.name} exists")

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
