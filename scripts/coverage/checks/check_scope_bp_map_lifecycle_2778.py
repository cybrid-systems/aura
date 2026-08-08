#!/usr/bin/env python3
"""Issue #2778: g_scope_bp_map lifecycle (erase / reset / LRU at cap).

#2633 residual: insert-only map leaked gauges until process restart.
After 256 distinct bp_scope_ids, new scopes silently fell back to the
process bucket — multi-tenant isolation failed without attribution.

Contract (one row per AC):
  AC1 erase_scope_bp_gauge + reset_scope_bp_map_for_test + size helper
  AC2 shared_ptr map (safe concurrent erase vs lookup/decay)
  AC3 at-cap note path LRU-evicts coldest last_event_us (not immortal drop)
  AC4 reset_all_agent_scopes_for_test clears g_scope_bp_map
  AC5 ac2778_* tests in test_mailbox_bp_admit; schema-2778 query keys
  AC6 this linter wired; no docs/design/2778-*; no test_issue_2778.cpp

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
    scope = _read("src/orch/agent_scope.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test = _read("tests/orch/test_mailbox_bp_admit.cpp")
    build = _read("build.py")

    # AC1 — free / reset / size API surface
    must("kMailboxBpScopeMapLifecycleIssue", "AC1", spawn)
    must("2778", "AC1", spawn)
    must("erase_scope_bp_gauge", "AC1", spawn)
    must("reset_scope_bp_map_for_test", "AC1", spawn)
    must("scope_bp_map_size_for_test", "AC1", spawn)

    # AC2 — shared_ptr (not unique_ptr) for concurrent-safe erase
    must("std::shared_ptr<ScopeBpGauge>", "AC2", spawn)
    must("std::make_shared<ScopeBpGauge>", "AC2", spawn)
    if "unique_ptr<ScopeBpGauge>" in spawn:
        fails.append("AC2: unique_ptr<ScopeBpGauge> still present (must be shared_ptr)")

    # AC3 — LRU at cap (coldest last_event_us)
    must("last_event_us", "AC3", spawn)
    must("coldest", "AC3", spawn)
    must("spawn_bp_scope_overflow_total", "AC3", spawn)

    # AC4 — reset_all_agent_scopes_for_test wires BP map clear
    must("reset_scope_bp_map_for_test", "AC4", scope)
    must("2778", "AC4", scope)

    # AC5 — tests + query surface
    must("ac2778_reset_clears_map", "AC5", test)
    must("ac2778_erase_one", "AC5", test)
    must("ac2778_lru_at_cap", "AC5", test)
    must("schema-2778", "AC5", prim)
    must("scope-bp-map-lifecycle-wired", "AC5", prim)
    must("scope-bp-map-size", "AC5", prim)

    # AC6 — linter wire + no design docs / no orphan test_issue file
    must("check_scope_bp_map_lifecycle_2778", "AC6", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2778-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "orch" / "test_issue_2778.cpp").is_file():
        fails.append("AC6: test_issue_2778.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2778 scope BP map lifecycle — erase/reset/LRU + "
        "reset_all_agent_scopes_for_test clears g_scope_bp_map + schema-2778"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
