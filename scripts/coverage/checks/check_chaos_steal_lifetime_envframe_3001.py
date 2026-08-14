#!/usr/bin/env python3
"""Issue #3001: chaos soak fail-closed on LifetimeProofOk + EnvFrameOk arms.

Residual of #2931/#2957: production soak must abort if residual
lifetime_proof_reject / envframe_lag / rearm_race grow without matching
RejectHard (no ticket). Soft AURA_STEAL_SNAPSHOT_SOFT=1: metric-only.
Do not invent a new soak — extend test_chaos_steal_mutation_gc +
test_steal_complete_restamp_txn. Inject via
g_steal_safety_between_clear_and_hard_and_hook.

Contract (one row per AC):
  AC1 Nightly / AURA_CHAOS_STEAL_GC=1 fail-closed if LifetimeProofOk or
      EnvFrameOk counters grow without matching RejectHard / no-ticket.
  AC2 Inject: negative last proof after densify → RejectHard, no ticket
      (production). Soft: metric-only, no abort.
  AC3 Default ./build.py test unaffected (EXCLUDE_FROM_ALL + env gate).
  AC4 #2931 keys remain fail-closed (additive).
  AC5 Source-cite + linter; extend chaos / restamp_txn (#81967).
  AC6 No docs/design/* per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    chaos = _read("tests/serve/test_chaos_steal_mutation_gc.cpp")
    txn = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    cpp = _read("src/serve/steal_safety.cpp")
    hdr = _read("src/serve/steal_safety.h")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 — soak fail-closed on the two residual arms + rearm.
    must("StealInvariant::LifetimeProofOk", "AC1", cpp)
    must("StealInvariant::EnvFrameOk", "AC1", cpp)
    must("g_steal_safety_residual_lifetime_proof_reject_total", "AC1", hdr)
    must("g_steal_safety_residual_envframe_lag_total", "AC1", hdr)
    must("g_steal_safety_residual_rearm_race_total", "AC1", hdr)
    must("residual_lifetime_proof_reject explained by RejectHard", "AC1 soak", chaos)
    must("residual_envframe_lag explained by RejectHard", "AC1 soak", chaos)
    must("residual_rearm_race explained by RejectHard", "AC1 soak", chaos)
    must("last_reject_invariant_bits", "AC1", chaos)
    must("Issue #3001", "AC1", cpp)
    must("Issue #3001", "AC1 hdr", hdr)

    # AC2 — hook inject + Soft no-abort.
    must("g_steal_safety_between_clear_and_hard_and_hook", "AC2", chaos)
    must("g_steal_safety_between_clear_and_hard_and_hook", "AC2 txn", txn)
    must("has_resume_safety_ticket()==false", "AC2", chaos)
    must("ac3001_1_hook_inject_negative_proof_rejects", "AC2", txn)
    must("ac3001_2_soft_metric_only", "AC2 Soft", txn)
    must("metric-only, no abort", "AC2 Soft soak", chaos)
    must("AURA_STEAL_SNAPSHOT_SOFT", "AC2 Soft", chaos)

    # AC3 — EXCLUDE_FROM_ALL + env gate unchanged.
    must("EXCLUDE_FROM_ALL", "AC3", cmake)
    must("AURA_CHAOS_STEAL_GC", "AC3", chaos)
    must("chaos_enabled", "AC3", chaos)
    if "set_target_properties(test_chaos_steal_mutation_gc PROPERTIES EXCLUDE_FROM_ALL TRUE)" not in cmake:
        fails.append("AC3: EXCLUDE_FROM_ALL not set on test_chaos_steal_mutation_gc")

    # AC4 — #2931 keys remain.
    must("#2931: residual_defer_after_exit explained by matching clears", "AC4", chaos)
    must("#2931: steal_safety_ticket_mismatch delta == 0", "AC4", chaos)
    must("#2931: resume_fence hard/ticket surplus == 0", "AC4", chaos)
    must("schema-2846", "AC4", chaos)
    must("ac3001_4_2931_keys_remain_fail_closed", "AC4", chaos)

    # AC5 — schema lineage + tests + linter wire.
    must("schema-2957", "AC5", q)
    must("schema-2901", "AC5", q)
    must("schema-2745", "AC5", q)
    must("schema-2957", "AC5 chaos", chaos)
    must("schema-2745", "AC5 chaos", chaos)
    must("schema-2901", "AC5 chaos", chaos)
    must("ac3001_1_lifetime_envframe_fail_closed", "AC5", chaos)
    must("ac3001_2_inject_negative_proof_reject_hard", "AC5", chaos)
    must("ac3001_5_source_and_linter", "AC5", chaos)
    must("ac3001_5_source_and_linter", "AC5 txn", txn)
    must("check_chaos_steal_lifetime_envframe_3001", "AC5", build)
    if (ROOT / "tests" / "serve" / "test_issue_3001.cpp").is_file():
        fails.append("AC5: test_issue_3001.cpp present (forbidden #81967)")

    # AC6
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*3001*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3001 chaos steal LifetimeProof/EnvFrame soak fail-closed — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
