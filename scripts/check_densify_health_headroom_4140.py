#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4140: query:arena-moving-densify-health planned_keys underheadroom.
# The Agent mutate-gate facade ran FlatHashTable::create(query_hash_
# capacity_for(84)) with 79 live insert_kv calls — live+8 = 87 > 84
# planned, violating the #3339 Agent decision headroom contract
# (planned >= live insert_kv + 8). The physical table was still sized from
# planned (capacity_for(84) -> 256) so keys were not dropped yet, but the
# next additive batch without a planned bump would approach overflow with
# no CI gate watching — and densify was the one Agent facade missing from
# the #3339 pin list (check_agent_decision_facade_headroom_3339.py).
#
# AC1 — the densify-health handler sizes its table from
#       kArenaMovingDensifyHealthPlannedKeys = 96 via
#       query_hash_capacity_for (no magic 84), live insert_kv (81 incl.
#       the schema-4140 / issue-4140 stamps) + 8 <= 96 holds, and the
#       handler keeps the bounded-probe overflowed=true contract feeding
#       query_hash_finish.
# AC2 — append-only keys: the legacy decision keys stay (no rename) —
#       production-hard-active, moving-window-green, would-allow-mutate,
#       schema-2619 / issue-2596 / schema-3200 / issue-3200 — and the
#       #4140 stamps sit at handler end (after issue-3200, before the
#       finish).
# AC3 — densify is pinned in the #3339 CI: the checker lists
#       query:arena-moving-densify-health with
#       kArenaMovingDensifyHealthPlannedKeys and enforces planned <
#       actual + 8 as a FAIL row (adversarial: +8 dummy insert_kv without
#       a planned raise must fail the gate).
# AC4 — Soft/Off unchanged: no densify query rename / -v2, and the runtime
#       ACs in test_engine_metrics_facade.cpp assert hash-overflow absent
#       under production_defaults (ac4140_2_no_overflow) and on the Soft
#       path (ac4140_4_soft), with densify in the production hard-fail
#       facade set.
# AC5 — ACs extend tests/compiler/test_engine_metrics_facade.cpp (per
#       #81934); no test_issue_4140.cpp; no docs/design/4140-* (per
#       #1655); linter wired in build.py + on the frozen allowlist.
#
# Issue #4284 is the same residual, filed against review tip 3534bec
# (live 79, capacity_for(84), densify absent from the #3339 pin list).
# The current handler already sizes from planned 96 and this linter
# plus check_agent_decision_facade_headroom_3339.py count insert_kv.
# Re-count must keep live+8 <= 96. No further bump, no appended keys,
# no query:4284, no test_issue_4284.cpp, no docs/design/4284-*.
#
# Self-test:
#   python3 scripts/check_densify_health_headroom_4140.py
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADROOM = 8
PLANNED = 96
INSERT_RE = re.compile(r'insert_kv(?:_str)?\(\s*"([^"]+)"')


def read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    obsjit = read("src/compiler/evaluator_primitives_obs_jit.cpp")
    checker = read("scripts/coverage/checks/check_agent_decision_facade_headroom_3339.py")
    test = read("tests/compiler/test_engine_metrics_facade.cpp")
    build = read("build.py")
    allow = read("scripts/coverage/root_check_allowlist.txt")
    densify_live = -1

    # AC1: planned constant + capacity helper + headroom math.
    must("constexpr std::size_t kArenaMovingDensifyHealthPlannedKeys = 96", "AC1 planned constant", obsjit)
    must("query_hash_capacity_for(kArenaMovingDensifyHealthPlannedKeys)", "AC1 capacity helper", obsjit)
    if "query_hash_capacity_for(84)" in obsjit:
        fails.append("AC1: magic capacity_for(84) still present")
    i = obsjit.find('"query:arena-moving-densify-health"')
    if i < 0:
        fails.append("AC1: densify handler not found")
    else:
        j = obsjit.find("return query_hash_finish", i)
        if j < 0:
            fails.append("AC1: densify handler finish not found")
        else:
            block = obsjit[i : j + 80]
            keys = INSERT_RE.findall(block)
            densify_live = len(keys)
            if densify_live + HEADROOM > PLANNED:
                fails.append(f"AC1: live {densify_live} + {HEADROOM} > planned {PLANNED}")
            must("bool overflowed = false", "AC1 overflowed flag", block)
            must("overflowed = true", "AC1 bounded insert stamps overflow", block)
            must("return query_hash_finish(ht, ev.string_heap_, overflowed)", "AC1 finish", block)
            # AC2: append-only — legacy keys preserved, stamps at end.
            for k in (
                "production-hard-active",
                "moving-window-green",
                "would-allow-mutate",
                "schema-2619",
                "issue-2596",
                "schema-3200",
                "issue-3200",
            ):
                must(f'insert_kv("{k}"', f"AC2 legacy key {k}", block)
            tail = block.rfind('insert_kv("schema-4140"')
            if tail < 0:
                fails.append("AC2: schema-4140 stamp missing")
            elif "return query_hash_finish" not in block[tail:]:
                fails.append("AC2: #4140 stamps not at handler end")
            must('insert_kv("issue-4140"', "AC2 issue-4140 stamp", block)

    # AC3: densify pinned in the #3339 CI.
    must("query:arena-moving-densify-health", "AC3 checker pins densify", checker)
    must("kArenaMovingDensifyHealthPlannedKeys", "AC3 checker pins planned constant", checker)
    must("planned < actual + HEADROOM", "AC3 checker enforces live+8<=planned", checker)

    # AC4: Soft/Off unchanged + runtime overflow ACs.
    if "query:arena-moving-densify-health-v2" in obsjit or "query:arena-moving-densify-health2" in obsjit:
        fails.append("AC4: densify query renamed (forbidden)")
    must("ac4140_2_no_overflow", "AC4 production overflow test", test)
    must("ac4140_4_soft", "AC4 Soft overflow test", test)
    must('"query:arena-moving-densify-health"', "AC4 densify in production hard-fail set", test)

    # AC5: no invent / wiring.
    if (ROOT / "tests" / "compiler" / "test_issue_4140.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_4140.cpp present (forbidden #81934)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4140-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    must("check_densify_health_headroom_4140", "AC5 build.py wiring", build)
    must("check_densify_health_headroom_4140.py", "AC5 allowlist entry", allow)

    # Issue #4284: pin the existing #4140 close. Recount live insert_kv;
    # do not require a planned bump past 96 or a new key.
    must("Issue #4140 / #4284", "4284: handler cites #4284 beside #4140", obsjit)
    must("kArenaMovingDensifyHealthPlannedKeys = 96", "4284: planned stays 96", obsjit)
    if densify_live < 0:
        fails.append("4284: densify live insert_kv was not counted")
    elif densify_live + HEADROOM > PLANNED:
        fails.append(f"4284: live {densify_live} + {HEADROOM} > planned {PLANNED}")
    must("query:arena-moving-densify-health", "4284: #3339 still pins densify", checker)
    must(
        "kArenaMovingDensifyHealthPlannedKeys",
        "4284: #3339 pins the planned constant",
        checker,
    )
    must("planned < actual + HEADROOM", "4284: #3339 fails when live+8 exceeds planned", checker)
    must("ac4140_2_no_overflow", "4284: production hash-overflow test retained", test)
    must("ac4140_4_soft", "4284: Soft overflow test retained", test)
    if "query:4284" in obsjit:
        fails.append("4284: new query key (forbidden)")
    if list((ROOT / "tests").rglob("test_issue_4284.cpp")):
        fails.append("4284: tests/**/test_issue_4284.cpp present (forbidden)")
    design = ROOT / "docs" / "design"
    if design.is_dir() and list(design.glob("4284-*")):
        fails.append("4284: docs/design/4284-* present (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #4140 densify-health planned_keys headroom — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
