#!/usr/bin/env python3
"""Issue #3026: force-JIT residual after reload-fail / storm-clear is
agent-actionable without permanent demotion (playbook observe-only).

Contract (one row per AC):
  AC1  residual_force_mask queryable after fail / only_covered leftover
  AC2  agent min-dirty + reemit coverage clears residual (no auto-heal)
  AC3  observe-only: no reemit in observe/decide; #2953 table unchanged
  AC4  Soft / Off: one production_defaults probe, no extra counters
  AC5  tests + build.py; no invent / docs/design; not a second playbook

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

    hh = _read("src/compiler/hot_update_registry.hh")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    bnd = _read("src/compiler/evaluator_mutation_boundary.cpp")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3026", "AC1", hh)
    must("residual_force_mask", "AC1 helper", hh)
    must("residual_force_mask", "AC1 impl", cpp)
    must("residual-force-mask", "AC1 recovery query", mut)
    must("residual-force-mask", "AC1 playbook query", mut)
    must("3026 AC1", "AC1 test", test)
    must("ac3026_1_residual_visible_until_covered", "AC1 test fn", test)

    # AC2
    must("playbook_hint_min_dirty_reemit", "AC2 hint", hh)
    must("playbook-hint-min-dirty-reemit", "AC2 query hint", mut)
    must("min-dirty residual", "AC2 playbook comment", hh)
    must("3026 AC2", "AC2 test", test)
    must("ac3026_2_agent_min_dirty_reemit_clears", "AC2 test fn", test)

    # AC3 observe-only + playbook table unchanged
    start = cpp.find("void HotUpdateRegistry::observe_residual_force_stale()")
    if start < 0:
        fails.append("AC3: observe_residual_force_stale missing")
    else:
        body = cpp[start : start + 900]
        if "aura_reemit_aot_for_dirty" in body:
            fails.append("AC3: observe calls reemit")
        if "on_reemit_pipeline_call" in body:
            fails.append("AC3: observe calls pipeline reemit")
        if "aura_reload_aot" in body:
            fails.append("AC3: observe calls reload")
    decide = cpp.find("aura_reload_recovery_playbook_decide")
    if decide < 0:
        fails.append("AC3: decide missing")
    else:
        dbody = cpp[decide : decide + 1800]
        if "aura_reemit_aot_for_dirty" in dbody:
            fails.append("AC3: decide calls reemit")
        if "playbook_hint_min_dirty_reemit" not in dbody:
            fails.append("AC3: decide missing additive hint")
        # Action table rows still present (priority comments).
        for row in ("reject-cross-ws", "wait-storm", "force-drain", "retry-reload", "reemit", "fall-back-jit"):
            if row not in dbody and row not in hh:
                fails.append(f"AC3: playbook row {row} missing")
    must("aura_hot_update_observe_residual_force_stale", "AC3 BoundaryExit", bnd)
    must("3026 AC3", "AC3 test", test)

    # AC4 Soft zero-cost
    must("aura_production_defaults_active_probe() == 0", "AC4 Soft skip", cpp)
    must("kStaleExits", "AC4 rate limit", cpp)
    must("3026 AC4", "AC4 test", test)
    must("ac3026_4_soft_zero_extra", "AC4 test fn", test)

    # AC5 wiring / no invent
    must("schema-3026", "AC5 schema query", mut)
    must("issue-3026", "AC5 issue query", mut)
    must("residual-force-stale-observe-total", "AC5 stale query", mut)
    must("residual-force-observe-wired", "AC5 wired query", mut)
    must("schema_3026", "AC5 snap field", hh)
    must("aura_hot_update_observe_residual_force_stale", "AC5 stub", stub)
    must("check_residual_force_agent_actionable_3026", "AC5 build", build)
    must("cmd_residual_force_agent_actionable_3026", "AC5 build cmd", build)
    must("ac3026_5_source_and_linter", "AC5 test fn", test)
    cite = hh.find("Issue #3026")
    if cite >= 0 and "AgentRegistry" in hh[cite : cite + 2500]:
        fails.append("AC5: must not introduce AgentRegistry")
    if (ROOT / "tests" / "compiler" / "test_issue_3026.cpp").is_file():
        fails.append("AC5: test_issue_3026.cpp present (forbidden per #81967)")
    if _read("docs/design/3026-residual-force-agent-actionable.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3026 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3026 residual force agent-actionable — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
