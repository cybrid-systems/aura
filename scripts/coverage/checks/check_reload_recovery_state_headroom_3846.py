#!/usr/bin/env python3
"""Issue #3846: query:reload-recovery-state joins #3339 Agent headroom gate.

Residual: planned 102 / ~95 live (headroom 7 < 8); local insert_kv (not
always insert_kv_checked). Blocked safely adding #3096 ResidualForceHeal
keys. Sibling #3847 published here as additive counters when headroom
allows (cite-only close for #3847).

Contract (one row per AC):
  AC1  kReloadRecoveryStatePlannedKeys + insert_kv_checked; planned >=
       live + 8; facade in #3339 check + test kFacades
  AC2  ResidualForceHeal counters published additively on state (+ playbook
       counters only): residual-force-auto-heal-total/wired, schema/issue-3096
  AC3  No rename of existing residual-force-* / recovery keys
  AC4  Extends test_engine_metrics_facade + #3339 linter; dedicated linter +
       grandfather + manifest + build.py; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HEADROOM = 8
MUT = "src/compiler/evaluator_primitives_mutate.cpp"
TEST = "tests/compiler/test_engine_metrics_facade.cpp"
CHECK3339 = "scripts/coverage/checks/check_agent_decision_facade_headroom_3339.py"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3846.json"
LINTER = "check_reload_recovery_state_headroom_3846"
INSERT_RE = re.compile(r'insert_kv(?:_str)?\(\s*"([^"]+)"')
PLANNED_RE = re.compile(
    r"constexpr std::size_t kReloadRecoveryStatePlannedKeys\s*=\s*(\d+)"
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _state_block(mut: str) -> str:
    """Builder body for reload-recovery-state (shared named lambda)."""
    pk = mut.find("constexpr std::size_t kReloadRecoveryStatePlannedKeys")
    if pk < 0:
        pk = mut.find("kReloadRecoveryStatePlannedKeys")
    if pk < 0:
        return ""
    end = mut.find("return query_hash_finish", pk)
    if end < 0:
        return ""
    return mut[pk : end + 40]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    mut = _read(MUT)
    test = _read(TEST)
    c3339 = _read(CHECK3339)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)
    evix = _read("src/compiler/evaluator.ixx")

    block = _state_block(mut)
    if not block:
        fails.append("AC1: reload-recovery-state PlannedKeys/builder not found")
    m = PLANNED_RE.search(block)
    keys = INSERT_RE.findall(block) if block else []
    actual = len(keys)
    planned = int(m.group(1)) if m else 0
    if planned < actual + HEADROOM:
        fails.append(
            f"AC1: planned {planned} < actual {actual} + {HEADROOM} headroom"
        )
    must("insert_kv_checked", "AC1 checked insert", block)
    must("kReloadRecoveryStatePlannedKeys = 112", "AC1 planned 112", mut)
    must("query:reload-recovery-state", "AC1 in #3339 facades", c3339)
    must("kReloadRecoveryStatePlannedKeys", "AC1 #3339 PlannedKeys name", c3339)
    must("query:reload-recovery-state", "AC1 test kFacades", test)
    must("kAgentDecisionFacadeHeadroom = 8", "AC1 headroom 8", evix)
    must("reload-recovery-state #3846", "AC1 evaluator.ixx cite", evix)

    # AC2 — #3096 / #3847 ResidualForceHeal keys on state (+ playbook)
    for k in (
        'insert_kv("residual-force-auto-heal-total"',
        'insert_kv("residual-force-auto-heal-wired"',
        'insert_kv("schema-3096"',
        'insert_kv("issue-3096"',
    ):
        must(k, "AC2 state heal key", block)
    must("residual-force-auto-heal-total", "AC2 playbook heal total", mut)
    must("schema-3096", "AC2 playbook schema-3096", mut)
    must("schema-3096", "AC2 test schema-3096", test)
    must("residual-force-auto-heal-wired", "AC2 test heal wired", test)
    must("#3847", "AC2 cites sibling #3847", mut + test + c3339)

    # AC3 — no rename of existing keys (spot-check stable names retained)
    for k in (
        "residual-force-mask",
        "residual-force-stale-observe-total",
        "residual-force-observe-wired",
        "schema-3026",
        "force-jit-regions-mask",
        "schema-2367",
    ):
        must(f'insert_kv("{k}"', f"AC3 retain {k}", block)
    must_not("query:reload-recovery-state-v2", "AC3 no rename", mut)
    must_not("query:aot-reload-recovery-stats-v2", "AC3 no alias rename", mut)

    # AC4 — wiring
    must(LINTER, "AC4 build registration", build)
    must("Issue #3846", "AC4 build cite", build)
    must(f"{LINTER}.py", "AC4 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC4 grandfather path", gf)
    must('"issue": 3846', "AC4 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC4: {MANIFEST} missing")
    must("check_agent_decision_facade_headroom_3339", "AC4 retains #3339", build)
    for rel in (
        "tests/compiler/test_issue_3846.cpp",
        "tests/core/test_issue_3846.cpp",
        "tests/issues/test_issue_3846.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3846*"):
            fails.append(f"AC4: docs/design/{p.name} exists")

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3846", file=sys.stderr)
        return 1

    print(
        f"OK {LINTER}: planned={planned} live={actual} "
        f"headroom={planned - actual} (#3096/#3847 keys included)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
