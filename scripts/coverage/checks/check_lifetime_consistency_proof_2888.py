#!/usr/bin/env python3
"""Issue #2888: unified LifetimeConsistencyProof coverage linter.

Contract (one row per AC):
  AC1  src/core/lifetime_consistency_proof.hh defines
       LifetimeConsistencyProof + make_lifetime_consistency_proof (pure
       aggregation of envframe + type-linear + pin + layout + residual) +
       stamp/poll helpers; would_allow_commit=true when all axes clean
  AC2  densify ownership scan fail / type-linear reject / residual fail →
       would_allow_commit=false + force_reason_code non-zero; Soft
       observes only (read-only aggregation, no rollback semantics)
  AC3  quiet path (no densify / no steal) → zero extra atomics; stamp
       sites only on outermost densify success + steal-complete;
       last-proof atomics stay healthy-empty
  AC4  query:lifetime-consistency-proof additive keys + schema-2888;
       existing #2711 / #2697 / pin stats surfaces preserved
  AC5  source-cite + tests in the existing src/-aligned densify suite
       (tests/compiler/test_densify_ownership_scan_fail_gate.cpp, #81967);
       no docs/design/ per #1655

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

FILES = [
    "src/core/lifetime_consistency_proof.hh",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/compiler/evaluator_primitives_query.cpp",
    "tests/compiler/test_densify_ownership_scan_fail_gate.cpp",
    "scripts/coverage/checks/check_lifetime_consistency_proof_2888.py",
]


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

    header = _read("src/core/lifetime_consistency_proof.hh")
    boundary = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    query = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_densify_ownership_scan_fail_gate.cpp")
    build = _read("build.py")

    # ── AC1: struct + pure aggregation + stamp/poll helpers ──
    must("struct LifetimeConsistencyProof", "AC1", header)
    must("make_lifetime_consistency_proof", "AC1", header)
    must("stamp_lifetime_consistency_proof", "AC1", header)
    must("last_lifetime_consistency_proof", "AC1", header)
    must("envframe_densify_scan_fail", "AC1", header)
    must("type_linear_outcome", "AC1", header)
    must("pin_contract_fail_total", "AC1", header)
    must("layout_arena_gen", "AC1", header)
    must("residual_defer_after_exit_total", "AC1", header)
    must("kLifetimeConsistencyProofIssue = 2888", "AC1", header)
    must("would_allow_commit =", "AC1", header)

    # ── AC2: fail axes → reject proof; Soft observe-only ──
    must("fail_envframe_scan", "AC2", header)
    must("kProofReasonEnvframeScanFail", "AC2", header)
    must("fail_type_linear", "AC2", header)
    must("kProofReasonTypeLinearReject", "AC2", header)
    must("fail_pin", "AC2", header)
    must("fail_residual", "AC2", header)
    must("never bumps counters", "AC2", header)

    # ── AC3: stamp sites only on densify success + steal-complete ──
    must("stamp_lifetime_consistency_proof(proof)", "AC3", boundary)
    must("stamp_lifetime_consistency_proof(proof)", "AC3", fiber)
    must("healthy-empty", "AC3", header)
    must("g_lcp_last_would_allow_commit", "AC3", header)

    # ── AC4: additive query surface + schema; existing preserved ──
    must("query:lifetime-consistency-proof", "AC4", query)
    must("lifetime-consistency-proof-would-allow-commit", "AC4", query)
    must("lifetime-consistency-proof-force-reason-code", "AC4", query)
    must("lifetime-consistency-proof-envframe-densify-scan-fail", "AC4", query)
    must("lifetime-consistency-proof-type-linear-outcome", "AC4", query)
    must("lifetime-consistency-proof-pin-contract-fail-total", "AC4", query)
    must("lifetime-consistency-proof-layout-arena-gen", "AC4", query)
    must("lifetime-consistency-proof-residual-defer-after-exit-total", "AC4", query)
    must("lifetime-consistency-proof-stamped-total", "AC4", query)
    must("schema-2888", "AC4", query)
    must("issue-2888", "AC4", query)
    # Regression: #2711 / #2697 surfaces intact.
    must("envframe-lifetime-proof-hold-gen", "AC4", query)
    must("schema-2711", "AC4", query)
    must("schema-2697", "AC4", query)

    # ── AC5: source-cite + tests in src/-aligned suite + build.py gate ──
    for rel in FILES:
        content = _read(rel)
        if not content:
            fails.append(f"AC5: missing file {rel}")
            continue
        if "Issue #2888" not in content:
            fails.append(f"AC5: {rel} does not cite Issue #2888")
    must("ac2888_1_header_and_aggregation", "AC5", test)
    must("ac2888_2_scan_fail_forces_reject", "AC5", test)
    must("ac2888_3_quiet_path_zero_cost", "AC5", test)
    must("ac2888_4_query_additive", "AC5", test)
    must("ac2888_5_source_and_linter", "AC5", test)
    must("check_lifetime_consistency_proof_2888", "AC5", build)
    # No docs/design per #1655.
    design_docs = sorted((ROOT / "docs" / "design").glob("2888-*")) if (ROOT / "docs" / "design").is_dir() else []
    if design_docs:
        fails.append(f"AC5: docs/design/2888-* present ({[p.name for p in design_docs]})")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_lifetime_consistency_proof_2888: {len(fails)} failure(s)")
        return 1

    print("check_lifetime_consistency_proof_2888: OK (AC1-AC5)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
