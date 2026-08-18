#!/usr/bin/env python3
"""Issue #3125: cross-scope directory merge — explicit span<AgentScope* const>.

C++ free function in src/orch/agent_scope.h that joins N per-Scope
directory snapshots into one CrossScopeSnapshot. Caller passes an
explicit span<AgentScope* const> (NOT a process-global registry walk).
Counters mirror into OrchModuleStats for dashboard observability.

Contract:
  AC1 agent_scope.h: kCrossScopeDirectoryIssue=3125 + CrossScopeEntry/
     Filter/Snapshot + cross_scope_directory(span<AgentScope* const>)
  AC2 agent_spawn.h OrchModuleStats: cross_scope_directory_total /
     entries_total / sources_total
  AC3 evaluator_primitives_agent.cpp facade: schema-3125 / issue-3125 /
     cross-scope-directory-wired + 3 cross-scope-directory-*-total keys
  AC4 src/orch/README.md orch section: "Cross-scope directory" with #3125
  AC5 3 test files extend existing ACs (test_agent_scope.cpp +
     test_agent_name_table_isolation.cpp + test_orch_scope.cpp)
  AC6 tests in src/-aligned suite (NOT tests/issues/test_issue_NNNN.cpp)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    scope_h = _read("src/orch/agent_scope.h")
    spawn_h = _read("src/orch/agent_spawn.h")
    prim = read_query_prims() + _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    test_scope = _read("tests/orch/test_agent_scope.cpp")
    test_iso = _read("tests/orch/test_agent_name_table_isolation.cpp")
    test_orch = _read("tests/orch/test_orch_scope.cpp")

    # AC1 — agent_scope.h surface
    must("kCrossScopeDirectoryIssue = 3125", "AC1", scope_h)
    must("struct CrossScopeEntry", "AC1", scope_h)
    must("struct CrossScopeFilter", "AC1", scope_h)
    must("struct CrossScopeSnapshot", "AC1", scope_h)
    must("cross_scope_directory(std::span<AgentScope* const>", "AC1", scope_h)
    must("source_path", "AC1", scope_h)
    must("source_seq", "AC1", scope_h)
    must("dedup_by_name", "AC1", scope_h)
    must("bp_scope_id()", "AC1", scope_h)

    # AC2 — OrchModuleStats counters (additive; siblings preserved)
    must("cross_scope_directory_total", "AC2", spawn_h)
    must("cross_scope_directory_entries_total", "AC2", spawn_h)
    must("cross_scope_directory_sources_total", "AC2", spawn_h)
    # sibling preservation: #2751 counters must remain
    must("agent_directory_total", "AC2 sibling #2751", spawn_h)
    must("agent_directory_entries_total", "AC2 sibling #2751", spawn_h)
    # Issue #3125 comment block references the contract
    must("Issue #3125", "AC2 comment", spawn_h)

    # AC3 — facade insert_kv keys (query:orch-module-stats surface)
    must_key("cross-scope-directory-total", "AC3", prim)
    must_key("cross-scope-directory-entries-total", "AC3", prim)
    must_key("cross-scope-directory-sources-total", "AC3", prim)
    must_key("schema-3125", "AC3", prim)
    must_key("issue-3125", "AC3", prim)
    must_key("cross-scope-directory-wired", "AC3", prim)
    must("kCrossScopeDirectoryIssue", "AC3 facade refs", prim)
    must("Issue #3125", "AC3 comment", prim)

    # AC4 — README orch section
    must("orch:cross-scope-directory", "AC4", readme)
    must("Issue #3125", "AC4", readme)
    must("Cross-scope directory merge", "AC4", readme)
    must("CrossScopeFilter", "AC4", readme)
    must("CrossScopeEntry", "AC4", readme)
    must("CrossScopeSnapshot", "AC4", readme)
    must("cross_scope_directory", "AC4", readme)
    must("bp_scope_id()", "AC4 source label", readme)
    must("dedup_by_name", "AC4", readme)
    must("no process-global registry", "AC4 #2751 contract preserved", readme)

    # AC5 — 3 test files extend existing ACs (no new test_issue_NNNN.cpp)
    must("ac3125_cross_scope_directory", "AC5 test_agent_scope", test_scope)
    must("ac3125_cross_scope_directory()", "AC5 test_scope call", test_scope)
    must("ac3125_cross_scope_isolation", "AC5 test_agent_name_table_isolation", test_iso)
    must("ac3125_cross_scope_isolation()", "AC5 test_iso call", test_iso)
    must("#3125: cross-scope directory merge", "AC5 test_orch_scope inline block", test_orch)
    must("kCrossScopeDirectoryIssue = 3125", "AC5 test_orch_scope source-cite", test_orch)
    must("cross_scope_directory(std::span<AgentScope* const>", "AC5 test_scope source-cite", test_scope)
    must("#3125", "AC5 test_iso source-cite", test_iso)

    # AC6 — no new tests/issues/test_issue_3125.cpp (per #81967 / AC6)
    issue_test = _read("tests/issues/test_issue_3125.cpp")
    if issue_test:
        fails.append("AC6: tests/issues/test_issue_3125.cpp exists (must NOT — extend src/-aligned suite per #81967)")

    if fails:
        print("check_cross_scope_directory_3125: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_cross_scope_directory_3125: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
