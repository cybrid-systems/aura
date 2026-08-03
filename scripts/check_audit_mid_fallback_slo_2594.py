#!/usr/bin/env python3
"""Issue #2594: audit mid-fallback 率 SLO → security-health 降级标志.

Coverage gate: presence-checks for the new gate + primitive + test +
build.py wiring + CMakeLists.txt test target. Mirrors
`check_parallel_isolation_level_2400.py` / `check_orch_mvp_scope.py` style.

Contract:
  AC1 primitive registered + reads audit_mid_fallback_gen_total
  AC2 production_defaults path arms (would_arm_degraded sentinel)
  AC3 normal mid path (low rate) does NOT breach
  AC4 soft mode observe-only (force-reason=soft-breach-observe, no arm)
  AC5 env override AURA_MID_FALLBACK_SLO_BP + schema-2594 sentinel

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    prim = _read("src/compiler/evaluator_primitives_security.cpp")
    hdr = _read("src/compiler/audit_mid_fallback_slo.h")
    test = _read("tests/compiler/test_audit_mid_fallback_slo_2594.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Header (pure gate contract).
    must("decide_audit_mid_fallback_slo", "header", hdr)
    must("evaluate_audit_mid_fallback_slo", "header", hdr)
    must("g_audit_mid_fallback_slo_counters", "header", hdr)
    must("reset_audit_mid_fallback_slo_for_test", "header", hdr)
    must("AURA_MID_FALLBACK_SLO_BP", "header", hdr)
    must("compute_mid_fallback_rate_bp", "header", hdr)
    must("MidFallbackSloInput", "header", hdr)
    must("MidFallbackSloDecision", "header", hdr)
    must("mid-fallback-slo-breach", "header", hdr)  # force_reason
    must("soft-breach-observe", "header", hdr)  # soft path
    must("would_arm_degraded", "header", hdr)  # struct field (header-only)

    # Primitive registration + wiring.
    must('"query:audit-mid-fallback-slo"', "prim", prim)
    must("audit_mid_fallback_gen_total", "prim", prim)
    must("contextual_total", "prim", prim)
    must("production_defaults_active", "prim", prim)
    must("AURA_SANDBOX", "prim", prim)
    must("evaluate_audit_mid_fallback_slo", "prim", prim)
    must("audit-mid-fallback-slo-wired", "prim", prim)
    must("schema-2594", "prim", prim)
    must("issue-2594", "prim", prim)
    must("rate-bp", "prim", prim)
    must("slo-bp", "prim", prim)
    must("breached", "prim", prim)
    must("would-arm-degraded", "prim", prim)
    must("soft-breach-observe-total", "prim", prim)
    must("arm-degraded-total", "prim", prim)
    must("last-rate-bp", "prim", prim)
    must("last-slo-bp", "prim", prim)
    must("last-breached", "prim", prim)
    must("last-would-arm-degraded", "prim", prim)
    # Header include (added to global module fragment).
    must("compiler/audit_mid_fallback_slo.h", "prim", prim)

    # Test file ACs.
    must("Issue #2594", "test", test)
    must("AC1", "test", test)
    must("AC2", "test", test)
    must("AC3", "test", test)
    must("AC4", "test", test)
    must("AC5", "test", test)
    must("decide_audit_mid_fallback_slo", "test", test)
    must("evaluate_audit_mid_fallback_slo", "test", test)
    must("reset_audit_mid_fallback_slo_for_test", "test", test)
    must("production_defaults_active", "test", test)
    must("resolve_audit_mutation_id", "test", test)
    must("audit_mid_fallback_gen_total", "test", test)
    must("mid-fallback-slo-breach", "test", test)
    must("soft-breach-observe", "test", test)
    must("schema-2594", "test", test)
    must("issue-2594", "test", test)
    must("compute_mid_fallback_rate_bp", "test", test)

    # build.py wiring.
    must("cmd_audit_mid_fallback_slo_2594_coverage", "build", build)
    must("check_audit_mid_fallback_slo_2594", "build", build)

    # CMakeLists.txt test target.
    must("aura_add_issue_test(test_audit_mid_fallback_slo_2594)", "cmake", cmake)
    must("aura_issue_test_link_light(test_audit_mid_fallback_slo_2594)", "cmake", cmake)
    must("all_test_issue_targets test_audit_mid_fallback_slo_2594", "cmake", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} audit-mid-fallback-slo (#2594) contract row(s) failed",
            file=sys.stderr,
        )
        return 1
    print("OK: Issue #2594 audit mid-fallback SLO — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
