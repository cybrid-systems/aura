#!/usr/bin/env python3
"""Issue #3841: quota-only recycle must not clear must_wait as cleaned.

maybe_force_release_reclaimed_quota releases quota only; callers must not
clear must_wait_reclaimed on success. Auto-wait must not exit Done solely
from force-release; abandon_reclaimed gates on owed cleanup (must_wait OR
deferred+quota_recycled_pending), not must_wait alone. Soft/Off helper
stays production-gated.

Contract:
  AC1  ensure / batch do not clear must_wait after force-release; helper
       sets quota_recycled_pending; issue stamp present
  AC2  auto-wait Done exit requires !must_wait (quota alone cannot clear);
       abandon gate uses deferred && quota_recycled_pending defense
  AC3  Soft helper production-gated unchanged
  AC4  suite rows in test_join_drain_reclaim.cpp; linter + grandfather +
       manifest + build.py; no invent (#81967 / #1655)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SPAWN = "src/orch/agent_spawn.h"
TEST = "tests/orch/test_join_drain_reclaim.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
LINTER = "check_quota_recycle_must_wait_ssot_3841"


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
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)

    # AC1 — stamp + keep must_wait + flag.
    must("kQuotaRecycleMustWaitSsotIssue = 3841", "AC1 stamp", spawn)
    must("Issue #3841", "AC1 cite", spawn)
    must("quota_recycled_pending", "AC1 flag", spawn)
    must("h.quota_recycled_pending = true;", "AC1 helper sets flag", spawn)
    must(
        "if (wr.status == serve::JoinStatus::Timeout)\n        (void)maybe_force_release_reclaimed_quota(h);",
        "AC1 ensure no must_wait clear",
        spawn,
    )
    must(
        "(void)maybe_force_release_reclaimed_quota(a);",
        "AC1 batch no must_wait clear",
        spawn,
    )
    # Forbidden: old clear-as-cleaned pattern adjacent to force-release.
    must_not(
        "if (wr.status == serve::JoinStatus::Timeout && maybe_force_release_reclaimed_quota(h))\n"
        "        h.must_wait_reclaimed = false;",
        "AC1 forbid ensure clear-as-cleaned",
        spawn,
    )
    must_not(
        "if (maybe_force_release_reclaimed_quota(a))\n                a.must_wait_reclaimed = false;",
        "AC1 forbid batch clear-as-cleaned",
        spawn,
    )

    # AC2 — auto-wait / abandon SSOT.
    must(
        "must_wait stays true after quota-only recycle",
        "AC2 auto-wait Done cite",
        spawn,
    )
    must(
        "h.reclaimed_deferred_cleanup && h.quota_recycled_pending",
        "AC2 abandon deferred+recycled gate",
        spawn,
    )
    must("slot_is_abandoned_live", "AC2 abandoned-live shape still present", spawn)

    # AC3 — Soft / production gate on helper unchanged.
    must(
        "if (!aura::compiler::typed_audit::production_defaults_active())\n        return false;",
        "AC3 helper production gate",
        spawn,
    )
    must("ac3841_5_soft_and_source_cite", "AC3 Soft test", test)

    # AC4 — tests + wiring + no invent.
    must("ac3841_1_ensure_keeps_must_wait_after_quota_recycle", "AC4 test AC1", test)
    must("ac3841_2_auto_wait_not_done_from_quota_alone", "AC4 test AC2", test)
    must("ac3841_3_abandon_after_quota_recycle_not_invalid", "AC4 test AC3", test)
    must("ac3841_4_batch_keeps_must_wait_after_quota_recycle", "AC4 test AC4", test)
    must("ac3841_5_soft_and_source_cite", "AC4 test AC5", test)
    must(LINTER, "AC4 build registration", build)
    must("Issue #3841", "AC4 build cite", build)
    must(f"{LINTER}.py", "AC4 grandfather basename", gf)
    must(
        f"scripts/coverage/checks/{LINTER}.py",
        "AC4 grandfather path",
        gf,
    )
    if not (ROOT / "scripts" / "coverage" / "manifests" / "3841.json").is_file():
        fails.append("AC4: scripts/coverage/manifests/3841.json missing")
    for rel in ("tests/orch/test_issue_3841.cpp", "tests/issues/test_issue_3841.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: {rel} exists")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3841*"):
            fails.append(f"AC4: docs/design/{p.name} exists")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    must_not("query:quota-recycle-must-wait", "AC4 no new query key", prim)

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
