#!/usr/bin/env python3
"""Issue #3337: Scope BP map overflow residual after #3127.

Production AgentScope dtor erases its named gauge. At-cap production
does not LRU-evict live tenants — new named scopes go to the overflow
bucket. Soft/Off keep LRU-evict + insert (zero extra). Process-bucket
"-" unchanged.

Contract (one row per AC):
  AC1  production AgentScope dtor / maybe_erase_scope_bp_gauge_on_teardown
       clears named gauges (map pressure drops after churn)
  AC2  at-cap production: live tenant gauges stay; new scope not inserted
  AC3  existing overflow / 2778 / process-bucket keys preserved
  AC4  Soft/Off: no erase lock/atomic; LRU path unchanged
  AC5  extend test_mailbox_bp_admit + test_per_scope_bp_admit;
       linter after #2778; no invent / no docs/design

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
    scope = _read("src/orch/agent_scope.h")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    test_bp = _read("tests/orch/test_mailbox_bp_admit.cpp")
    test_per = _read("tests/orch/test_per_scope_bp_admit.cpp")
    lint2778 = _read("scripts/coverage/checks/check_scope_bp_map_lifecycle_2778.py")
    lint3127 = _read("scripts/coverage/checks/check_mailbox_bp_overflow_3127.py")
    build = _read("build.py")

    must("kMailboxBpScopeOverflowTeardownIssue = 3337", "AC1 stamp", spawn)
    must("maybe_erase_scope_bp_gauge_on_teardown", "AC1 helper", spawn)
    must("maybe_erase_scope_bp_gauge_on_teardown(bp_scope_id_)", "AC1 dtor", scope)
    must("scope_bp_gauge_teardown_erase_total{0}", "AC1 counter END", spawn)
    raw_pos = spawn.find("agent_send_raw_held_ref_total{0}")
    erase_pos = spawn.find("scope_bp_gauge_teardown_erase_total{0}")
    if erase_pos < 0 or raw_pos < 0 or erase_pos < raw_pos:
        fails.append("AC1: teardown-erase counter must END-append after #3336")
    must("ac3337_1_teardown_helper", "AC1 test", test_bp)
    must("ac3337_1_scope_dtor_clears_gauge", "AC1 dtor test", test_per)

    note_pos = spawn.find("inline void note_mailbox_bp_recent_event")
    next_fn = spawn.find("inline std::shared_ptr<ScopeBpGauge> lookup_scope_bp_gauge", note_pos)
    note = spawn[note_pos:next_fn] if next_fn > note_pos else spawn[note_pos:]
    prod = note.find("if (production_defaults_active())")
    evict = note.find("g_scope_bp_map.erase(coldest)")
    if prod < 0 or evict < 0 or prod > evict:
        fails.append("AC2: production overflow return must precede Soft LRU-evict")
    must("ac3337_2_live_tenant_not_evicted", "AC2 test", test_bp)

    must("spawn_bp_scope_overflow_dropped_total", "AC3 #3127 counter", spawn)
    must("kBpScopeProcessBucket", "AC3 process-bucket", spawn)
    must("erase_scope_bp_gauge", "AC3 #2778 erase retained", spawn)
    must("schema-2778", "AC3 schema-2778", prim)
    must("schema-3337", "AC3 additive schema", prim)

    must("ac3337_4_soft_quiet_no_erase", "AC4 test", test_bp)
    must("if (!production_defaults_active())", "AC4 Soft skip", spawn)
    must("return false;", "AC4 Soft skip return", spawn)

    must("check_scope_bp_overflow_teardown_3337", "AC5 build.py", build)
    must("check_scope_bp_map_lifecycle_2778", "AC5 #2778 wired", build)
    must("Issue #2778", "AC5 #2778 linter retained", lint2778)
    must("Issue #3127", "AC5 #3127 linter retained", lint3127)
    must("ac3337_5_no_invent", "AC5 test", test_bp)
    prev = build.find("check_scope_bp_map_lifecycle_2778")
    ours = build.find("check_scope_bp_overflow_teardown_3337")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #2778")
    if (ROOT / "tests" / "orch" / "test_issue_3337.cpp").is_file():
        fails.append("AC5: tests/orch/test_issue_3337.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3337-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3337 scope BP overflow teardown — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
