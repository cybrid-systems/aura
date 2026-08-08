#!/usr/bin/env python3
"""Issue #2633: scope-local mailbox BP recent gauge (multi-tenant isolation).

Contract (one row per AC):
  AC1 AgentSpec::bp_scope_id field exists in src/orch/agent_spawn.h
  AC2 note_mailbox_bp_recent_event(scope_id) overload (default empty)
  AC3 spawn_agent_with_mailbox prefers scope-local gauge when bp_scope_id set
  AC4 maybe_decay_mailbox_bp_recent decays per-bucket under map mutex
      (#2780: no snapshot-then-zero race; skip active last_event_us)
  AC5 spawn_bp_admit_reject_scope_total counter in OrchModuleStats
  AC6 spawn_bp_scope_overflow_total counter in OrchModuleStats
  AC7 ScopeBpGauge struct + g_scope_bp_map bounded (cap kMailboxBpScopeMapCap)
  AC8 :bp-scope-id kw arg wired into orch:spawn-agent primitive
  AC9 linter wired into build.py + test coverage in src-aligned suite

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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
    build = _read("build.py")

    # AC1
    must("bp_scope_id", "AC1", spawn)
    must("Issue #2633", "AC1", spawn)
    must("std::string bp_scope_id{}", "AC1", spawn)

    # AC2
    must("note_mailbox_bp_recent_event(std::string_view", "AC2", spawn)
    must("Issue #2633", "AC2", spawn)

    # AC3
    must("lookup_scope_bp_gauge", "AC3", spawn)
    must("scope_active", "AC3", spawn)
    must("Issue #2633", "AC3", spawn)

    # AC4 — per-bucket decay under map mutex (#2780 race fix replaces
    # the prior snapshot.reserve + unlock + zero pattern).
    must("maybe_decay_mailbox_bp_recent", "AC4", spawn)
    must("Issue #2633", "AC4", spawn)
    must("g_scope_bp_map_mtx", "AC4", spawn)
    # #2780: skip active scopes by last_event_us (not snapshot-then-zero).
    must("last_event_us", "AC4", spawn)

    # AC5
    must("spawn_bp_admit_reject_scope_total", "AC5", spawn)
    must("Issue #2633", "AC5", spawn)

    # AC6
    must("spawn_bp_scope_overflow_total", "AC6", spawn)
    must("Issue #2633", "AC6", spawn)

    # AC7
    must("kMailboxBpScopeMapCap", "AC7", spawn)
    must("ScopeBpGauge", "AC7", spawn)
    must("g_scope_bp_map", "AC7", spawn)
    must("g_scope_bp_map_mtx", "AC7", spawn)

    # AC8
    must("bp-scope-id", "AC8", prim)
    must("bp_scope_id", "AC8", prim)
    must("Issue #2633", "AC8", prim)
    must("spec.bp_scope_id = std::move(bp_scope_id)", "AC8", prim)

    # AC9
    must("check_scope_bp_gauge_coverage", "AC9", build)

    # cross-check: stamp-resolve --strict must still be green (no regression)
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / "check_stamp_resolve_coverage.py"), "--strict"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"stamp-resolve --strict regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: scope-local BP gauge #2633 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
