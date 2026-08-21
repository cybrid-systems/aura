#!/usr/bin/env python3
"""Issue #3195: production multi-worker residual-zero sticky + Soft misconfig.

Under multi-worker Ready, residual counters must not stay metric-only.
Latch production multi-worker so steal_safety_production_residual_zero_v_read
(SSOT) fail-closes on named residuals and sticky is set. Soft / single-worker
/ unit-test (latch unset): zero behavioural change.

Contract:
  AC1 Hard: multi-worker latch + any named residual (BoundaryUnsafe /
      LifetimeProof / LayoutStamp / GcDefer / EnvFrame) → sticky + SSOT 0
  AC2 residual_zero remains SSOT; Soft / single-worker / unit-test unchanged
  AC3 ABI self-check requires residual-zero sticky wiring (bit 5)
  AC4 Additive only; reuse sticky + residual counters; no g_3195_*
  AC5 Extend steal-safety / production residual suites; no invent / docs
  AC6 Source-cite linter + build.py

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

    sh = _read("src/serve/steal_safety.h")
    sc = _read("src/serve/steal_safety.cpp")
    hh = _read("src/serve/runtime_production_abi.h")
    abi = _read("src/serve/runtime_production_abi.cpp")
    fiber = _read("src/serve/fiber.cpp")
    qts = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    t_zero = _read("tests/serve/test_steal_safety_production_residual_zero.cpp")
    t_abi = _read("tests/serve/test_steal_complete_strong_entry.cpp")
    build = _read("build.py")

    fn_pos = sc.find("StealSafetyDecision steal_safety_transaction(")
    fn_win = sc[fn_pos : fn_pos + 9000] if fn_pos >= 0 else ""

    # AC1
    must("kStealSafetyProductionMultiWorkerResidualStickyIssue = 3195", "AC1 stamp", sh)
    must("aura_runtime_multi_worker_production_latched", "AC1 latch consult", sh)
    must("g_steal_safety_residual_boundary_unsafe_total", "AC1 BoundaryUnsafe", sh)
    must("g_steal_safety_residual_layout_stamp_mismatch_total", "AC1 LayoutStamp", sh)
    must("g_steal_safety_residual_gc_defer_armed_total", "AC1 GcDefer", sh)
    must("g_steal_safety_residual_envframe_lag_total", "AC1 EnvFrame", sh)
    must("g_steal_safety_residual_lifetime_proof_reject_total", "AC1 LifetimeProof", sh)
    must("Issue #3195", "AC1 transaction cite", fn_win)
    must("aura_runtime_multi_worker_production_latched() != 0", "AC1 force sticky", fn_win)
    must("g_steal_safety_production_residual_sticky_fail.store(1", "AC1 sticky store", fn_win)

    # AC2
    must("steal_safety_production_residual_zero_v_read", "AC2 SSOT", sh)
    must("!multi && aura_production_defaults_active_probe() == 0", "AC2 Soft unlatched", sh)
    must("3195 AC2", "AC2 live test", t_abi)

    # AC3
    must("kProductionAbiSelfcheckFailBitResidualSticky", "AC3 bit 5", hh)
    must("g_production_multi_worker_latched{0}", "AC3 latch", hh)
    must("g_steal_safety_production_residual_sticky_fail_wired", "AC3 sticky wired", abi)
    must("g_steal_safety_production_residual_zero_wired", "AC3 residual-zero wired", abi)
    must("g_production_multi_worker_latched.store(1", "AC3 Ready latch", abi)
    must("steal_safety_production_residual_zero_v_read() == 0", "AC3 Ready SSOT", abi)
    must("aura_runtime_multi_worker_production_latched", "AC3 weak stub", fiber)

    # AC4
    must("g_steal_safety_production_residual_sticky_fail", "AC4 reuse sticky", sh)
    if "g_3195_" in sh or "g_3195_" in sc or "g_3195_" in hh or "g_3195_" in abi:
        fails.append("AC4: new g_3195_* counter (reuse existing)")

    # AC5 / AC6
    must("AC10 (Issue #3195)", "AC5 residual-zero suite", t_zero)
    must("AC13: live residual_zero latch", "AC5 residual-zero live", t_zero)
    must("3195 AC1", "AC5 strong-entry live", t_abi)
    must("schema-3195", "AC5 schema", qts)
    must("issue-3195", "AC5 issue stamp", qts)
    must("check_production_multi_worker_residual_sticky_3195", "AC6 build.py", build)

    if (ROOT / "tests" / "issues" / "test_issue_3195.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3195.cpp per #81967")
    if (ROOT / "tests" / "serve" / "test_issue_3195.cpp").is_file():
        fails.append("AC5: forbidden tests/serve/test_issue_3195.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3195-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3195 production_multi_worker_residual_sticky:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3195 production_multi_worker_residual_sticky: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
