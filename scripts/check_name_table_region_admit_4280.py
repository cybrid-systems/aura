#!/usr/bin/env python3
"""Issue #4280: bare orch:spawn-agent admits N keyless mutate agents.

AgentScope::spawn denies that shape (#4238). The name-table plane did
not, so hosts on orch:spawn-agent got parallel fibers and a silent
Serialized mutate on agent_apply_mu_.

Fix shape (no second isolation model; keys are never auto-invented):
  - orch:spawn-agent, under production_defaults_active, walks THIS
    Evaluator's AgentNameTable (append_live_region_keys) plus the
    candidate and calls decide_isolation + region_key_missing_serialized
    + parallel_require_region_keys_deny.
  - Deny returns before spawn_agent_with_mailbox / put. Counter reuses
    region_key_missing_serialized_total. deny-detail is the existing
    missing-or-overlap-keys string. No new query key.
  - AgentHandle stores region_key (END field, move ctor and assign).
  - Soft / Off: the production load is the gate; no getenv on Soft.
  - Scope #4238 gate stays. RestartN is not this plane.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    def window(src: str, begin: str, end: str, label: str, width: int = 4000) -> str:
        b = src.find(begin)
        if b == -1:
            fails.append(f"{label}: anchor {begin!r} missing")
            return ""
        e = src.find(end, b + len(begin))
        return src[b : (e if e != -1 else b + width)]

    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    spawn = _read("src/orch/agent_spawn.h")
    table = _read("src/compiler/agent_name_table.h")
    scope = _read("src/orch/agent_scope.h")
    test = _read("tests/orch/test_agent_name_table_isolation.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    gate = window(
        prim,
        "Issue #4280: name-table plane shares the #4238 admit predicate.",
        "orch_sched.ensure(2);",
        "AC1",
    )
    must("production_defaults_active()", "AC1", gate)
    must("append_live_region_keys", "AC1", gate)
    must("decide_isolation", "AC1", gate)
    must("region_key_missing_serialized", "AC1", gate)
    must("parallel_require_region_keys_deny", "AC1", gate)
    must("region_key_missing_serialized_total", "AC1", gate)
    must("region-key-missing spawn deny (#4280)", "AC1", gate)
    must("kSerializedReasonMissingOrOverlapKeys", "AC1", gate)
    must("return build_orch_hash(rkv);", "AC1", gate)
    absent("spawn_agent_with_mailbox", "AC1", gate)
    absent("class AgentRegistry", "AC1", prim)
    absent("auto_region_key", "AC1", prim)
    absent("query:4280", "AC1", prim)

    must("h.region_key = spec.region_key", "AC2", spawn)
    must("region_key(o.region_key)", "AC2", spawn)
    must("region_key = o.region_key", "AC2", spawn)
    must("append_live_region_keys", "AC2", table)
    must("slot.region_key", "AC2", table)

    must("region_key_missing_admit_deny_unlocked_", "AC3", scope)

    must("static void ac4280_name_table_region_key_admit()", "AC4", test)
    must("ac4280_name_table_region_key_admit();", "AC4", test)
    for ac in ("4280 AC1", "4280 AC2", "4280 AC3", "4280 AC4", "4280 AC5", "4280 AC6"):
        must(ac, "AC4", test)
    if _read("tests/orch/test_issue_4280.cpp"):
        fails.append("AC4: tests/orch/test_issue_4280.cpp must not exist")
    if _read("docs/design/4280-name-table-region-admit.md"):
        fails.append("AC4: docs/design/4280-* must not exist")

    must("check_name_table_region_admit_4280", "AC5", build)
    must("check_name_table_region_admit_4280.py", "AC5", allow)

    if fails:
        print("check_name_table_region_admit_4280: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_name_table_region_admit_4280: ok (AC1–AC5)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
