#!/usr/bin/env python3
"""Issue #2957: residual hard-AND arm (f) consults last LifetimeConsistencyProof.

Contract:
  AC1 production + fresh negative proof after densify → RejectHard, no ticket
  AC2 Soft / no densify / would_allow → no new rejects; zero cost when unset
  AC3 existing arms a–e + #2901 re-arm preserved
  AC4 additive query keys; #2888 non-regressing
  AC5 source-cite + tests + linter; no invent test file
  AC6 no docs/design/*
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

    hdr = _read("src/serve/steal_safety.h")
    cpp = _read("src/serve/steal_safety.cpp")
    lcp = _read("src/core/lifetime_consistency_proof.hh")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    qt = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    test = _read("tests/serve/test_steal_complete_restamp_txn.cpp")
    build = _read("build.py")

    # AC1
    must("LifetimeProofOk", "AC1", hdr)
    must("g_steal_safety_residual_lifetime_proof_reject_total", "AC1", hdr)
    must("kStealSafetyLifetimeProofResidualIssue", "AC1", hdr)
    must("StealInvariant::LifetimeProofOk", "AC1", cpp)
    must("last_lifetime_consistency_proof_present", "AC1", cpp)
    must("last_lifetime_consistency_would_allow", "AC1", cpp)
    must("is_steal_snapshot_hard_mode", "AC1", cpp)
    must("last_densify_call_seq", "AC1", cpp)
    must("last_lifetime_consistency_would_allow", "AC1", lcp)
    must("last_lifetime_consistency_proof_present", "AC1", lcp)
    must("ac2957_1_production_negative_proof_rejects", "AC1", test)

    # AC2
    must("Soft", "AC2", cpp)
    must("stamped_total", "AC2 soft quiet path", lcp)
    must("ac2957_2_soft_and_quiet_no_reject", "AC2", test)

    # AC3
    must("StealInvariant::BoundarySafe", "AC3", cpp)
    must("StealInvariant::EnvFrameOk", "AC3", cpp)
    must("g_steal_safety_residual_rearm_race_total", "AC3", hdr)
    must("ac2957_3_prior_arms_preserved", "AC3", test)

    # AC4
    must("schema-2957", "AC4", q)
    must("steal-safety-residual-lifetime-proof-reject-total", "AC4", q)
    must("steal-invariant-lifetime-proof-fail-total", "AC4", q)
    must("schema-2888", "AC4", q)
    must("query:lifetime-consistency-proof", "AC4", qt)
    must("schema-2929", "AC4 lineage", q)
    must("ac2957_4_query_additive", "AC4", test)

    # AC5 / AC6
    must("check_steal_lifetime_proof_residual_2957", "AC5", build)
    must("ac2957_5_source_and_linter", "AC5", test)
    must("Issue #2957", "AC5", cpp)
    if (ROOT / "tests" / "serve" / "test_issue_2957.cpp").is_file():
        fails.append("AC5: test_issue_2957.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2957-*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2957 steal lifetime-proof residual arm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
