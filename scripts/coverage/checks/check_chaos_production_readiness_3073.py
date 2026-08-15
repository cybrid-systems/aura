#!/usr/bin/env python3
"""Issue #3073: production soak readiness gate.

Binds steal residual-zero (including LifetimeProof + EnvFrame) and
hold-after-cancel max ≤ #3071 bound into one fail-closed soak invariant.
Soft / unit: counters only; no abort. Reuses existing counters.

Contract (one row per AC):
  AC1 Production soak: residual envframe/life delta == 0 and
     max hold-after-cancel ≤ bound (window still open); #2755 four-arm
     residual-zero preserved.
  AC2 Soft / unit default: no abort (prod_ready_gate requires
     residual_zero_gate + production_defaults_active).
  AC3 Reuse existing counters; additive schema-3073 only.
  AC4 Wire into pre-push / gate (coverage linter).
  AC5 Extend chaos residual_zero suite (#81967); no invent test file.
  AC6 No docs/design/* per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

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

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    chaos = _read("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
    mhb = _read("src/compiler/mutation_hold_budget.h")
    hdr = _read("src/serve/steal_safety.h")
    q = read_query_prims()
    build = _read("build.py")

    must("Issue #3073", "AC1", chaos)
    must("prod_ready_gate", "AC1 named gate", chaos)
    must("steal_safety_residual_envframe_lag_total_v_read", "AC1 EnvFrame", chaos)
    must("steal_safety_residual_lifetime_proof_reject_total_v_read", "AC1 LifetimeProof", chaos)
    must("max_hold_after_cancel_us", "AC1 max hold-after-cancel", chaos)
    must("#3073: residual_envframe_lag delta == 0", "AC1 EnvFrame abort", chaos)
    must("#3073: residual_lifetime_proof_reject delta == 0", "AC1 LifetimeProof abort", chaos)
    must("#3073: max hold-after-cancel exceeds bound", "AC1 hold max abort", chaos)
    must("#2755: residual_boundary_unsafe delta == 0", "AC1 #2755 preserved", chaos)
    must("g_steal_safety_residual_envframe_lag_total", "AC1 header EnvFrame", hdr)
    must("g_steal_safety_residual_lifetime_proof_reject_total", "AC1 header LifetimeProof", hdr)

    must("production_defaults_active()", "AC2", chaos)
    must("print only", "AC2 Soft/unit", chaos)
    must("residual_zero_gate", "AC2 residual_zero still gates", chaos)

    must("kChaosProductionReadinessIssue = 3073", "AC3", mhb)
    must_key("schema-3073", "AC3", q)
    must_key("issue-3073", "AC3", q)
    must_key("production-readiness-soak-gate-wired", "AC3", q)
    must_key("schema-3071", "AC3 preserved", q)
    must("g_steal_safety_residual_boundary_unsafe_total", "AC3 #2755 counter", hdr)

    must("check_chaos_production_readiness_3073", "AC4", build)
    must("ac3073_1_production_soak_binds_residual_and_hold", "AC5", chaos)
    must("ac3073_2_soft_unit_no_abort", "AC5", chaos)
    must("ac3073_3_reuse_counters_additive", "AC5", chaos)
    must("ac3073_4_source_linter_gate", "AC5", chaos)
    if (ROOT / "tests" / "serve" / "test_issue_3073.cpp").is_file():
        fails.append("AC5: tests/serve/test_issue_3073.cpp present (forbidden invent)")

    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("3073-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3073 production soak readiness gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
