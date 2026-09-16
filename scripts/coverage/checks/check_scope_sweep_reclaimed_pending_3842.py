#!/usr/bin/env python3
"""Issue #3842: AgentScope::sweep_reclaimed_pending for Scope-owned vector hosts.

Long-lived C++ hosts that keep AgentHandle in Scope (spawn registers into
handles_) without ensure_reclaimed_cleanup / wait / abandon must not bottom
out at ~AgentHandle. Scope.sweep drains Reclaimed-pending via the ensure
SSOT. Soft: no new force path. No process-global AgentRegistry. Aura orch:*
auto-wait unchanged.

Contract:
  AC1  sweep_reclaimed_pending API + ensure SSOT + Soft production gate
  AC2  spawn registers with Scope lifetime; no AgentRegistry invent
  AC3  Soft zero-cost (production gate early return)
  AC4  suite rows in test_join_drain_reclaim.cpp; linter + grandfather +
       manifest + build.py; no invent (#81967 / #1655)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SPAWN = "src/orch/agent_spawn.h"
SCOPE = "src/orch/agent_scope.h"
TEST = "tests/orch/test_join_drain_reclaim.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
LINTER = "check_scope_sweep_reclaimed_pending_3842"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    spawn = _read(SPAWN)
    scope = _read(SCOPE)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)

    # AC1 — stamp + sweep API + ensure SSOT.
    must("kScopeSweepReclaimedPendingIssue = 3842", "AC1 stamp", spawn)
    must("Issue #3842", "AC1 cite spawn", spawn)
    must("Issue #3842", "AC1 cite scope", scope)
    must("sweep_reclaimed_pending()", "AC1 sweep API", scope)
    must("SweepReclaimedPendingResult", "AC1 result type", scope)
    must("ensure_reclaimed_cleanup(h)", "AC1 ensure SSOT", scope)
    must(
        "if (!aura::compiler::typed_audit::production_defaults_active())\n"
        "            return out;",
        "AC1 Soft production gate",
        scope,
    )
    # Soft: no new force path on the sweep surface (ensure SSOT may still
    # call #3529 elsewhere; the #3842 method body must not invent one).
    sweep_idx = scope.find("sweep_reclaimed_pending()")
    if sweep_idx < 0:
        fails.append("AC1: sweep_reclaimed_pending() missing for region check")
    else:
        sweep_region = scope[sweep_idx : sweep_idx + 2500]
        if "maybe_force_release_reclaimed_quota" in sweep_region:
            fails.append("AC1: sweep body must not call maybe_force_release_reclaimed_quota")
        if "AgentRegistry" in sweep_region and "No process-global AgentRegistry" not in sweep_region and "no process-global AgentRegistry" not in sweep_region:
            # Allow forbid-cite; forbid inventing a registry type in the region.
            if "class AgentRegistry" in sweep_region or "struct AgentRegistry" in sweep_region:
                fails.append("AC1: sweep region invents AgentRegistry")

    # AC2 — spawn registers; no AgentRegistry invent (type symbols only;
    # header comments may cite the forbidden identifiers).
    must("registers the raw AgentHandle with this Scope", "AC2 spawn register cite", scope)
    must_not("class AgentRegistry", "AC2 forbid class AgentRegistry", scope)
    must_not("struct AgentRegistry", "AC2 forbid struct AgentRegistry", scope)
    must_not("class AgentRegistry", "AC2 forbid AgentRegistry in spawn", spawn)
    must_not("struct AgentRegistry", "AC2 forbid struct AgentRegistry in spawn", spawn)

    # AC3 — Soft test present.
    must("ac3842_3_soft_zero_cost_no_force", "AC3 Soft test", test)

    # AC4 — tests + wiring + no invent.
    must("ac3842_1_scope_sweep_cleans_vector_host_after_body_exit", "AC4 test AC1", test)
    must("ac3842_2_scope_sweep_timeout_keeps_pending_no_force", "AC4 test AC2", test)
    must("ac3842_3_soft_zero_cost_no_force", "AC4 test AC3", test)
    must("ac3842_4_spawn_registers_with_scope_lifetime", "AC4 test AC4", test)
    must("ac3842_5_source_cite_linter_no_invent", "AC4 test AC5", test)
    must(LINTER, "AC4 build registration", build)
    must("Issue #3842", "AC4 build cite", build)
    must(f"{LINTER}.py", "AC4 grandfather basename", gf)
    must(
        f"scripts/coverage/checks/{LINTER}.py",
        "AC4 grandfather path",
        gf,
    )
    if not (ROOT / "scripts" / "coverage" / "manifests" / "3842.json").is_file():
        fails.append("AC4: scripts/coverage/manifests/3842.json missing")
    for rel in ("tests/orch/test_issue_3842.cpp", "tests/issues/test_issue_3842.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC4: {rel} exists")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3842*"):
            fails.append(f"AC4: docs/design/{p.name} exists")
    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    must_not("query:scope-sweep-reclaimed", "AC4 no new query key", prim)
    # Aura auto-wait path unchanged (still delegates to ensure SSOT).
    must("maybe_auto_wait_reclaimed_production", "AC4 auto-wait present", spawn)
    must("ensure_reclaimed_cleanup(h)", "AC4 auto-wait ensure SSOT", spawn)

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
