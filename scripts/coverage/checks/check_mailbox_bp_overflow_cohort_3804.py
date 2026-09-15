#!/usr/bin/env python3
"""Issue #3804: production BP scope overflow cohort must not share admit
pressure across unrelated named scopes after g_scope_bp_map hits
kMailboxBpScopeMapCap (256).

Under Restricted+production, new named scopes that cannot insert still
bump spawn_bp_scope_overflow_* for dashboards (#3127 note path), but
load_mailbox_bp_recent returns 0 for missing named scopes (no shared
g_scope_bp_overflow.recent). Spawn fail-closes with typed BpAdmit and
deny-detail mailbox-bp-scope-overflow. Soft/Off LRU path unchanged.

Contract:
  AC1  production overflow cohort: fail-closed typed BpAdmit with
       distinct deny-detail; no shared admit pressure
  AC2  spawn_bp_scope_overflow_* keys retained; Agents distinguish
       overflow-cohort deny from scope-local deny
  AC3  Soft/Off LRU path unchanged (zero-cost)
  AC4  soft soak: storming overflow peer does not BpAdmit-deny an
       in-map quiet peer via shared gauge; quiet overflow gets typed deny
  AC5  extend test_mailbox_bp_admit; linter wired; no invent / no docs

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
    test_bp = _read("tests/orch/test_mailbox_bp_admit.cpp")
    lint3127 = _read("scripts/coverage/checks/check_mailbox_bp_overflow_3127.py")
    build = _read("build.py")

    must("kMailboxBpScopeOverflowCohortIssue = 3804", "AC1 stamp", spawn)
    must("named_scope_bp_on_overflow_cohort", "AC1 helper", spawn)
    must("mailbox-bp-scope-overflow", "AC1 deny-detail", spawn)
    must("Issue #3804", "AC1 cite", spawn)

    load_pos = spawn.find("[[nodiscard]] inline std::uint64_t load_mailbox_bp_recent")
    cohort_pos = spawn.find(
        "[[nodiscard]] inline bool named_scope_bp_on_overflow_cohort", load_pos
    )
    load_fn = spawn[load_pos:cohort_pos] if cohort_pos > load_pos else spawn[load_pos:]
    must("return 0;", "AC1 load missing→0", load_fn)
    if "g_scope_bp_overflow.recent.load" in load_fn:
        fails.append("AC1: load must not share g_scope_bp_overflow.recent")

    admit_pos = spawn.find("named_scope_bp_on_overflow_cohort(scope_id)")
    if admit_pos < 0:
        fails.append("AC1: admit path must call named_scope_bp_on_overflow_cohort")
    else:
        admit_snip = spawn[admit_pos : admit_pos + 900]
        must('quota_dimension = "mailbox-bp-scope-overflow"', "AC1 admit detail", admit_snip)
        must("AgentDenyClass::BpAdmit", "AC1 typed BpAdmit", admit_snip)
        must("spawn_bp_scope_overflow_total.fetch_add", "AC2 overflow counter on deny", admit_snip)

    must("spawn_bp_scope_overflow_total", "AC2 overflow_total key", spawn)
    must("spawn_bp_scope_overflow_dropped_total", "AC2 dropped key retained", spawn)
    must("spawn-bp-scope-overflow-total", "AC2 query key retained", prim)
    must("schema-3804", "AC2 schema", prim)
    must("scope-bp-overflow-cohort-wired", "AC2 wired flag", prim)

    note_pos = spawn.find("inline void note_mailbox_bp_recent_event")
    next_fn = spawn.find("inline std::shared_ptr<ScopeBpGauge> lookup_scope_bp_gauge", note_pos)
    note = spawn[note_pos:next_fn] if next_fn > note_pos else spawn[note_pos:]
    prod = note.find("if (production_defaults_active())")
    evict = note.find("g_scope_bp_map.erase(coldest)")
    if prod < 0 or evict < 0 or prod > evict:
        fails.append("AC3: Soft LRU-evict must remain after production overflow return")
    must("ac3804_3_soft_lru_inserts", "AC3 Soft test", test_bp)
    must("ac3804_3_soft_not_overflow_cohort", "AC3 Soft cohort false", test_bp)

    must("ac3804_4_quiet_load_no_crosstalk", "AC4 crosstalk test", test_bp)
    must("ac3804_4_in_map_quiet_admits_despite_overflow_storm", "AC4 in-map quiet", test_bp)
    must("ac3804_1_overflow_cohort_fail_closed", "AC4/AC1 fail-closed", test_bp)
    must("ac3804_2_distinct_deny_detail", "AC2 deny-detail test", test_bp)
    must("ac3804_2_scope_local_reject_untouched", "AC2 scope reject untouched", test_bp)

    must("check_mailbox_bp_overflow_cohort_3804", "AC5 build.py", build)
    must("Issue #3804", "AC5 #3127 linter cites residual", lint3127)
    must("ac3804_5_no_invent", "AC5 no invent", test_bp)
    if (ROOT / "tests" / "orch" / "test_issue_3804.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_3804.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3804-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    prev = build.find("check_agent_scope_region_key_isolation_3803")
    ours = build.find("check_mailbox_bp_overflow_cohort_3804")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3803")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3804 BP scope overflow cohort isolation — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
