#!/usr/bin/env python3
"""Issue #3339: Agent decision facade planned_keys headroom + no overflow.

Agent single-hash decision facades must keep
planned_keys >= actual insert_kv count + 8. Additive insert_kv must
raise planned_keys. hash-overflow on these facades is a hard fail
(non-Agent catalogs may still stamp #3020 overflow).

Contract (one row per AC):
  AC1  planned >= actual + 8 on evolution-audit-decision /
       security-posture / type-linear-commit-health /
       type-linear-evolution-snapshot / reload-recovery-playbook
  AC2  tests under production_defaults assert hash-overflow is absent
  AC3  +20 dummy keys without raising planned would fail AC1 on
       evolution-audit-decision
  AC4  Soft / no new query key names; #3020 linter retained
  AC5  linter after #3020; tests in test_engine_metrics_facade;
       no invent / no docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HEADROOM = 8
INSERT_RE = re.compile(r'insert_kv(?:_str)?\(\s*"([^"]+)"')
PLANNED_RE = re.compile(r"constexpr std::size_t (\w+PlannedKeys)\s*=\s*(\d+)")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _block(src: str, query: str) -> str:
    needle = f'"{query}"'
    i = src.find(needle)
    if i < 0:
        return ""
    ends: list[int] = []
    for tok in ("return query_hash_finish", "return make_hash"):
        j = src.find(tok, i + 20)
        if j > i:
            ends.append(j)
    end = min(ends) + 40 if ends else min(len(src), i + 25000)
    return src[i:end]


def _actual(block: str) -> list[str]:
    return INSERT_RE.findall(block)


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ev = _read("src/compiler/evaluator.ixx")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    ref = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_engine_metrics_facade.cpp")
    lint3020 = _read("scripts/coverage/checks/check_query_hash_overflow_3020.py")
    build = _read("build.py")

    must("kAgentDecisionFacadeHeadroom = 8", "AC1 headroom constant", ev)
    must("kAgentDecisionFacadeHeadroomIssue = 3339", "AC1 stamp", ev)
    must("Additive insert_kv must raise planned_keys", "AC1 comment", ev)

    facades = [
        ("query:evolution-audit-decision", sec, "kEvolutionAuditDecisionPlannedKeys"),
        ("query:security-posture", sec, "kSecurityPosturePlannedKeys"),
        ("query:type-linear-commit-health", ref, "kTypeLinearCommitHealthPlannedKeys"),
        ("query:type-linear-evolution-snapshot", ref, "kTypeLinearEvolutionSnapshotPlannedKeys"),
        ("query:reload-recovery-playbook", mut, "kReloadRecoveryPlaybookPlannedKeys"),
    ]

    evo_planned = 0
    evo_actual = 0
    for query, src, planned_name in facades:
        block = _block(src, query)
        if not block:
            fails.append(f"AC1: {query} handler not found")
            continue
        keys = _actual(block)
        actual = len(keys)
        m = PLANNED_RE.search(block)
        if not m or m.group(1) != planned_name:
            fails.append(f"AC1: {query} missing {planned_name}")
            continue
        planned = int(m.group(2))
        if planned < actual:
            fails.append(f"AC1: {query} planned {planned} < actual {actual}")
        elif planned < actual + HEADROOM:
            fails.append(f"AC1: {query} planned {planned} < actual {actual} + {HEADROOM} headroom")
        must("query_hash_capacity_for", f"AC1 {query} capacity helper", block)
        must("insert_kv_checked", f"AC1 {query} checked insert", block)
        must("query_hash_finish", f"AC1 {query} finish", block)
        if query == "query:evolution-audit-decision":
            evo_planned = planned
            evo_actual = actual
            must('insert_kv("schema-3114"', "AC1 evolution schema-3114", block)
            must('insert_kv("suggested-next-code"', "AC1 suggested-next-code", block)
            must('insert_kv("last-audit-mid"', "AC1 last-audit-mid", block)
        if query == "query:security-posture":
            must('insert_kv("schema-2534"', "AC1 posture schema-2534", block)

    obs_block = _block(obs, "query:security-posture")
    must("kSecurityPostureWalPlannedKeys", "AC1 obs posture planned", obs_block)
    obs_keys = _actual(obs_block)
    om = PLANNED_RE.search(obs_block)
    if om:
        op = int(om.group(2))
        oa = len(obs_keys)
        if op < oa + HEADROOM:
            fails.append(f"AC1: obs security-posture planned {op} < actual {oa} + {HEADROOM}")

    must("ac3339_2_no_overflow", "AC2 test", test)
    must("hash-overflow", "AC2 test overflow key", test)
    must("apply_production_audit_defaults", "AC2 production", test)

    if evo_actual > 0 and evo_planned >= (evo_actual + 20) + HEADROOM:
        fails.append(
            f"AC3: evolution planned {evo_planned} absorbs +20 dummy keys "
            f"(actual {evo_actual}); keep slack < 20+{HEADROOM}"
        )
    must("ac3339_3_plus20", "AC3 test cite", test)

    must("check_query_hash_overflow_3020", "AC4 #3020 linter retained", build)
    must("Issue #3020", "AC4 #3020 linter body", lint3020)
    must("engine:metrics", "AC4 facade unchanged", test)
    must("ac3339_4_soft", "AC4 Soft test", test)

    must("check_agent_decision_facade_headroom_3339", "AC5 build.py", build)
    prev = build.find("check_query_hash_overflow_3020")
    ours = build.find("check_agent_decision_facade_headroom_3339")
    if ours < 0:
        fails.append("AC5: linter must be wired in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3020")
    must("ac3339_5_no_invent", "AC5 test", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3339.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3339.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3339-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3339 Agent decision facade headroom — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
