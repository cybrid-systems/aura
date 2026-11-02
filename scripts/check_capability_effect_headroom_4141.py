#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4141: query:capability-effect-stats planned_keys headroom.
# The highest-churn Agent security stats facade ran FlatHashTable::create(
# query_hash_capacity_for(186)) with 177 live insert_kv calls — live+8 =
# 185 <= 186 is barely compliant with headroom 9, so ONE additive key
# without a planned bump breaches the #3339 Agent decision headroom
# contract (planned >= live insert_kv + 8) and can stamp hash-overflow,
# silently dropping Agent-visible counters (grant-mid-refused,
# durable-grant denies, ...). capability-effect-stats was the last Agent
# decision facade missing from the #3339 pin list
# (check_agent_decision_facade_headroom_3339.py).
#
# AC1 — the capability-effect-stats handler sizes its table from
#       kCapabilityEffectStatsPlannedKeys = 208 via
#       query_hash_capacity_for (no magic 186), live insert_kv (179 incl.
#       the schema-4141 / issue-4141 stamps) + 8 <= 208 holds, and the
#       handler keeps the bounded-probe overflowed=true contract feeding
#       query_hash_finish.
# AC2 — append-only keys: the legacy decision keys stay (no rename) —
#       schema / active / phase / enforced / denied / provenance-mismatch /
#       mid-join-zero-deny / mid-join-fail-closed-armed / schema-2707 /
#       issue-2707 / schema-3971 / issue-3971 — and the #4141 stamps sit
#       at handler end (after issue-3971, before the finish).
# AC3 — capability-effect-stats is pinned in the #3339 CI: the checker
#       lists query:capability-effect-stats with
#       kCapabilityEffectStatsPlannedKeys and enforces planned < actual +
#       8 as a FAIL row.
# AC4 — Soft/Off unchanged: no capability-effect query rename / -v2, and
#       the runtime ACs in test_engine_metrics_facade.cpp assert
#       hash-overflow absent under production_defaults
#       (ac4141_2_no_overflow) and on the Soft path (ac4141_4_soft), with
#       capability-effect-stats in the production hard-fail facade set.
# AC5 — ACs extend tests/compiler/test_engine_metrics_facade.cpp (per
#       #81934); no test_issue_4141.cpp; no docs/design/4141-* (per
#       #1655); linter wired in build.py + on the frozen allowlist.
#
# Issue #4285 is the same near-breach, filed against review tip 3534bec
# (177 live / planned 186, headroom 9). #4141 raised planned to 192.
# Re-count at the current tip is 179 live, so live+16 = 195 and 192 is
# short of the requested buffer. Planned is 208 (no new keys). The
# #3339 checker still fails when planned < actual + 8. No query:4285,
# no test_issue_4285.cpp, no docs/design/4285-*.
#
# Self-test:
#   python3 scripts/check_capability_effect_headroom_4141.py
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADROOM = 8
PLANNED = 208
BUFFER = 16
INSERT_RE = re.compile(r'insert_kv(?:_str)?\(\s*"([^"]+)"')


def read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    sec = read("src/compiler/evaluator_primitives_security.cpp")
    checker = read("scripts/coverage/checks/check_agent_decision_facade_headroom_3339.py")
    test = read("tests/compiler/test_engine_metrics_facade.cpp")
    build = read("build.py")
    allow = read("scripts/coverage/root_check_allowlist.txt")
    effect_live = -1

    # AC1: planned constant + capacity helper + headroom math.
    must(
        "constexpr std::size_t kCapabilityEffectStatsPlannedKeys = 208",
        "AC1 planned constant",
        sec,
    )
    must(
        "query_hash_capacity_for(kCapabilityEffectStatsPlannedKeys)",
        "AC1 capacity helper",
        sec,
    )
    if "query_hash_capacity_for(186)" in sec:
        fails.append("AC1: magic capacity_for(186) still present")
    i = sec.find('"query:capability-effect-stats"')
    if i < 0:
        fails.append("AC1: capability-effect-stats handler not found")
    else:
        j = sec.find("return query_hash_finish", i)
        if j < 0:
            fails.append("AC1: capability-effect handler finish not found")
        else:
            block = sec[i : j + 80]
            keys = INSERT_RE.findall(block)
            effect_live = len(keys)
            if effect_live + HEADROOM > PLANNED:
                fails.append(f"AC1: live {effect_live} + {HEADROOM} > planned {PLANNED}")
            must("bool overflowed = false", "AC1 overflowed flag", block)
            must("overflowed = true", "AC1 bounded insert stamps overflow", block)
            must("return query_hash_finish(ht, ev.string_heap_, overflowed)", "AC1 finish", block)
            # AC2: append-only — legacy keys preserved, stamps at end.
            for k in (
                "schema",
                "active",
                "phase",
                "enforced",
                "denied",
                "provenance-mismatch",
                "mid-join-zero-deny",
                "mid-join-fail-closed-armed",
                "schema-2707",
                "issue-2707",
                "schema-3971",
                "issue-3971",
            ):
                must(f'insert_kv("{k}"', f"AC2 legacy key {k}", block)
            tail = block.rfind('insert_kv("schema-4141"')
            if tail < 0:
                fails.append("AC2: schema-4141 stamp missing")
            elif "return query_hash_finish" not in block[tail:]:
                fails.append("AC2: #4141 stamps not at handler end")
            must('insert_kv("issue-4141"', "AC2 issue-4141 stamp", block)

    # AC3: capability-effect-stats pinned in the #3339 CI.
    must("query:capability-effect-stats", "AC3 checker pins capability-effect", checker)
    must("kCapabilityEffectStatsPlannedKeys", "AC3 checker pins planned constant", checker)
    must("planned < actual + HEADROOM", "AC3 checker enforces live+8<=planned", checker)

    # AC4: Soft/Off unchanged + runtime overflow ACs.
    if "query:capability-effect-stats-v2" in sec or "query:capability-effect-stats2" in sec:
        fails.append("AC4: capability-effect query renamed (forbidden)")
    must("ac4141_2_no_overflow", "AC4 production overflow test", test)
    must("ac4141_4_soft", "AC4 Soft overflow test", test)
    must(
        '"query:capability-effect-stats"',
        "AC4 capability-effect in production hard-fail set",
        test,
    )

    # AC5: no invent / wiring.
    if (ROOT / "tests" / "compiler" / "test_issue_4141.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_4141.cpp present (forbidden #81934)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4141-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    must("check_capability_effect_headroom_4141", "AC5 build.py wiring", build)
    must("check_capability_effect_headroom_4141.py", "AC5 allowlist entry", allow)

    # Issue #4285: recount and keep the live+16 buffer. No new key.
    must("Issue #4141 / #4285", "4285: handler cites #4285 beside #4141", sec)
    must("kCapabilityEffectStatsPlannedKeys = 208", "4285: planned is 208", sec)
    if effect_live < 0:
        fails.append("4285: capability-effect live insert_kv was not counted")
    elif effect_live + BUFFER > PLANNED:
        fails.append(f"4285: live {effect_live} + {BUFFER} > planned {PLANNED}")
    must("query:capability-effect-stats", "4285: #3339 still pins capability-effect", checker)
    must(
        "kCapabilityEffectStatsPlannedKeys",
        "4285: #3339 pins the planned constant",
        checker,
    )
    must("planned < actual + HEADROOM", "4285: #3339 fails when live+8 exceeds planned", checker)
    must("ac4141_2_no_overflow", "4285: production hash-overflow test retained", test)
    must("ac4141_4_soft", "4285: Soft overflow test retained", test)
    must("ac4285_recount", "4285: facade test cites the recount", test)
    must("ac4285_no_invent", "4285: facade test forbids a new issue test", test)
    if "query:4285" in sec:
        fails.append("4285: new query key (forbidden)")
    if list((ROOT / "tests").rglob("test_issue_4285.cpp")):
        fails.append("4285: tests/**/test_issue_4285.cpp present (forbidden)")
    design = ROOT / "docs" / "design"
    if design.is_dir() and list(design.glob("4285-*")):
        fails.append("4285: docs/design/4285-* present (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #4141 capability-effect-stats planned_keys headroom — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
