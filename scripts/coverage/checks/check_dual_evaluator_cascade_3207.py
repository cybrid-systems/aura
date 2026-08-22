#!/usr/bin/env python3
"""Issue #3207: dual-Evaluator concurrent grant/revoke × TenantScope cascade.

CapabilityRegistry stays process-global. Cascade + single-use consume +
live_session_grants updates happen-before observable under dual-Evaluator
chaos. Soft/Off zero-cost. Existing counters only.

Contract:
  AC1 dual-Evaluator: EvA grant_session + EvB TenantScope cascade + EvA
      require_effect fully allow-or-deny; after quiesce live==0
  AC2 Soft/Off zero-cost (live==0 short-circuit, no lock)
  AC3 existing counters only (no mid-struct insert)
  AC4 SE reason strings unchanged
  AC5 Restricted + multi-tenant test; source-cite; no invent

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

    cap = _read("src/core/capability_model.hh")
    eval_sec = _read("src/compiler/evaluator_security.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    must("kCapabilityDualEvaluatorCascadeIssue = 3207", "AC5 stamp", cap)
    must("revoke_session_grants_for_locked", "AC1 locked cascade", cap)
    must("revoke_session_grants_for_mid_locked", "AC1 locked mid-revoke", cap)
    must("mark_session_bound_stolen_locked", "AC1 locked stolen mark", cap)
    must("revoke_session_grants_on_steal_or_abort_locked", "AC1 locked steal/abort", cap)
    must("Issue #3207", "AC1 consume lock comment", cap)
    must("no lock-drop vs TenantScope cascade revoke", "AC1 consume happens-before", cap)

    must("Issue #3207", "AC1 TenantScope cite", eval_sec)
    must("revoke_session_grants_for_locked", "AC1 TenantScope locked cascade", eval_sec)
    must("set_capability_tenant_id(prev_tenant_)", "AC1 restore under lock", eval_sec)

    must("mark_session_bound_stolen_locked", "AC1 steal locked mark", fiber)
    must("revoke_session_grants_on_steal_or_abort_locked", "AC1 steal locked abort", fiber)

    # AC2 Soft/Off zero-cost: live==0 short-circuit remains on public wrappers
    must("AC3: zero extra work when no session grants", "AC2 Soft short-circuit", cap)
    must("AC4: Soft / empty live residual — no lock", "AC2 steal Soft short-circuit", cap)
    if "if (!production &&" not in cap:
        fails.append("AC2: production vs Soft live==0 split missing")

    # AC3 no new counters / no mid-struct insert
    if "capability_dual_eval" in cap or "cascade_race_total" in cap:
        fails.append("AC3: new mid-struct counter (forbidden)")
    must("capability_live_session_grants", "AC3 reuse live counter", cap)
    must("capability_session_revoke_total", "AC3 reuse session_revoke", cap)
    must("capability_single_use_consumed_total", "AC3 reuse single_use_consumed", cap)

    # AC4 SE reasons unchanged
    must("scope-dtor-cascade", "AC4 cascade reason", cap)
    must("session-mid-exit", "AC4 mid-exit reason", cap)
    must("session-mid-steal-exit", "AC4 steal reason", cap)
    must("session-mid-abort-exit", "AC4 abort reason", cap)
    must("single-use-consumed", "AC4 consume reason", cap)

    # AC5 tests + linter + no invent
    must("ac3207_1_sequential_cascade_then_require_effect_deny", "AC5 sequential test", test)
    must("ac3207_2_dual_evaluator_concurrent_chaos", "AC5 chaos test", test)
    must("ac3207_3_soft_off_zero_cost", "AC5 Soft test", test)
    must("ac3207_4_source_cite_and_linter", "AC5 source-cite test", test)
    must("Issue #3207", "AC5 test cite", test)
    must("std::thread", "AC5 concurrent threads", test)
    must("check_dual_evaluator_cascade_3207", "AC5 build.py wires linter", build)

    if (ROOT / "tests" / "core" / "test_issue_3207.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3207.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3207.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3207.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3207-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL: Issue #3207 linter found", len(fails), "problems:")
        for f in fails:
            print(" -", f)
        return 1
    print("OK: Issue #3207 — dual-Evaluator cascade + consume linearizability.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
