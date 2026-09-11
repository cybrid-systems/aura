#!/usr/bin/env python3
# scripts/check_reclaimed_slot_second_recycle_3644.py -- Issue #3644 source-cite gate.
#
# AC1: done-body arm — the resolution planes run the second recycle
#      (maybe_force_recycle_reclaimed_slot) BEFORE the #3564 quota arm;
#      a stuck done body routes complete_agent_join_cleanup(Ok)
#      (mailbox detach + pending flags cleared → slot retires clean,
#      same-name put passes). #3564's helper never touches done bodies,
#      so no #3564 AC sees this arm.
# AC2: live-body arm — abandon shape (mailbox detach + reset, name +
#      both pending flags cleared, body-stack never freed #2661) and it
#      fires only after the quota is gone (reserved_memory_bytes == 0),
#      so the #3564 first-visit semantics (quota released, flags stay,
#      #3467 deny) are unchanged.
# AC3: gates — pending flags + production_defaults_active +
#      reclaimed_quota_stuck_past_timeout; Soft / Off / not stuck:
#      no-op (false). Call sites: name-table find + put walk + Scope
#      find, each falling through to the #3564 quota arm.
# AC4: counters — existing keys only: reclaimed_quota_force_released_
#      total (done arm) + reclaimed_abandon_total (live arm); no new
#      query key; no docs/design/3644*; no tests/**/test_issue_3644.cpp.
# AC5: suite rows (ac3644 in tests/orch/test_join_drain_reclaim.cpp) +
#      build.py registration.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SPAWN = "src/orch/agent_spawn.h"
NAME_TABLE = "src/compiler/agent_name_table.h"
SCOPE = "src/orch/agent_scope.h"
PRIM = "src/compiler/evaluator_primitives_agent.cpp"
TEST = "tests/orch/test_join_drain_reclaim.cpp"
BUILD = "build.py"

LINTER = "check_reclaimed_slot_second_recycle_3644"
HELPER = "maybe_force_recycle_reclaimed_slot"
QUOTA = "maybe_force_release_reclaimed_quota"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(spawn: str, name_table: str, scope: str, prim: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — done-body arm: full Done-path cleanup on a stuck done body.
    must(HELPER, "AC1 sibling helper", spawn)
    must("Issue #3644", "AC1 cite", spawn)
    must("kReclaimedSlotSecondRecycleIssue = 3644", "AC1 issue constant", spawn)
    must("if (h.fiber && h.fiber->is_done()) {", "AC1 done-body gate", spawn)
    must("complete_agent_join_cleanup(h, done_jr);", "AC1 done-body cleanup", spawn)

    # AC2 — live-body arm: abandon shape, only after the quota is gone.
    must("if (h.reserved_memory_bytes != 0)", "AC2 quota-gone gate", spawn)
    must("h.mailbox->detach(h.fiber);", "AC2 mailbox detach", spawn)
    must("h.mailbox.reset();", "AC2 mailbox reset", spawn)
    must("h.name.clear();", "AC2 name clear", spawn)
    must("h.must_wait_reclaimed = false;", "AC2 must_wait clear", spawn)
    must("h.reclaimed_deferred_cleanup = false;", "AC2 deferred clear", spawn)
    must_not("delete h.fiber", "AC2 no body-stack free", spawn)

    # AC3 — gates + resolution-plane call sites, two-step with #3564.
    must("production_defaults_active()", "AC3 production gate", spawn)
    must("reclaimed_quota_stuck_past_timeout(h.fiber)", "AC3 stuck gate", spawn)
    must(f"if (!aura::orch::{HELPER}(it->second))", "AC3 name-table find two-step", name_table)
    must(f"if (!aura::orch::{HELPER}(slot))", "AC3 name-table put walk two-step", name_table)
    must(f"if (!aura::orch::{HELPER}(h))", "AC3 scope find two-step", scope)
    must(QUOTA, "AC3 #3564 quota arm retained (name-table)", name_table)
    must(QUOTA, "AC3 #3564 quota arm retained (scope)", scope)

    # AC4 — counters: existing keys only; no new query key / docs / test_issue.
    must("reclaimed_quota_force_released_total.fetch_add", "AC4 done-arm counter", spawn)
    must("reclaimed_abandon_total.fetch_add", "AC4 live-arm counter", spawn)
    must_not("reclaimed-slot-second-recycle", "AC4 no new query key", prim)
    must_not("Issue #3644", "AC4 no prim churn", prim)
    if "docs/design/3644" in str(sorted(Path(ROOT / "docs" / "design").glob("3644*"))):
        fails.append("AC4: docs/design/3644-* exists")
    for rel in ("tests/orch/test_issue_3644.cpp", "tests/issues/test_issue_3644.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: {rel} exists")

    # AC5 — suite rows + build registration.
    must("ac3644_1_done_body_full_cleanup_on_find", "AC5 test AC1 row", test)
    must("ac3644_2_live_body_abandon_shape_on_later_find", "AC5 test AC2 row", test)
    must("ac3644_3_soft_no_recycle", "AC5 test AC3 row", test)
    must("ac3644_4_not_stuck_no_recycle_deny_and_wait_intact", "AC5 test AC4 row", test)
    must("ac3644_5_source_cite_and_no_invent", "AC5 test AC5 row", test)
    must(LINTER, "AC5 build registration", build)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue 3644 second-recycle gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    spawn = _read(SPAWN)
    name_table = _read(NAME_TABLE)
    scope = _read(SCOPE)
    prim = _read(PRIM)
    test = _read(TEST)
    build = _read(BUILD)
    if args.self_test:
        broken = spawn.replace(HELPER, "maybe_force_recycle_redacted")
        self_fails = _rows(broken, name_table, scope, prim, test, build)
        if not self_fails:
            print("self-test FAILED: helper removal undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(spawn, name_table, scope, prim, test, build)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
