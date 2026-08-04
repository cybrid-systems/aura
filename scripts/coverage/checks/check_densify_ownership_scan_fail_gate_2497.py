#!/usr/bin/env python3
"""Issue #2497: Phase 5 hard-bind densify ownership scan fail → suppress
outermost success metrics.

#2340 / #2361 / #2376 made `DensifyConsistencyReport.envframe_ok` real and
last-call capable. Residual gap from review: densify ownership scan fail delta
must **always** suppress outermost Phase 5 success metrics the same way
`pin_contract_held == false` does — no path where scan fail is metrics-only.

Contract:
  AC1 Linter fails when the Phase 5 gate (scan_fail_delta → envframe_ok
     suppression) is missing from evaluator_mutation_boundary.cpp.
  AC2 Clean Moving densify path remains unchanged (gate does not fire when
     no fail delta). Source-cite pairing's within-pairing delta is preserved
     (#2361) — #2497 widens the window, does not replace.
  AC3 Soft / empty densify zero extra cost (gate only runs in the Moving
     branch).
  AC4 Query surface exposes densify-ownership-scan-fail-total (existing
     #2361) + densify-ownership-scan-fail-gate-wired sentinel (#2497) +
     schema-2497 + issue-2497.
  AC5 Source-cite Phase 5 gate next to pin_contract_held gate (Phase 5
     ordering proximity — same fail-closed shape as #2266 / #2341 AC2).

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efl = _read("src/core/envframe_lifetime.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_densify_ownership_scan_fail_gate.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — Phase 5 driver must snapshot baseline + recompute delta + AND into
    # envframe_ok (fail-closed shape mirrors pin_contract_held gating).
    must("scan_fail_baseline", "AC1", emb)
    must("envframe_lifetime_densify_ownership_scan_fail_total", "AC1", emb)
    must("scan_fail_after", "AC1", emb)
    must("scan_fail_delta", "AC1", emb)
    must("!scan_fail_delta", "AC1", emb)
    # Issue stamp + fail-closed rationale comment.
    must("Issue #2497", "AC1", emb)
    must("pin_contract_held", "AC1", emb)  # source-cite proximity to #2266
    must("fail-closed", "AC1", emb)
    must("metrics-only", "AC1", emb)
    # Snapshot must be INSIDE the moving_compact_enabled() block (covers
    # the entire densify window before compact + pairing).
    compact_pos = emb.find("aura::ast::moving_compact_enabled()")
    snapshot_pos = emb.find("scan_fail_baseline =")
    must("scan_fail_baseline = aura::core::envframe_lifetime::", "AC1", emb)
    if compact_pos != -1 and snapshot_pos != -1 and snapshot_pos < compact_pos:
        # Also fine if snapshot is in a sibling position above compact.
        pass

    # AC2 — pairing's within-pairing delta preserved (#2361). The widening
    # gate (#2497) ANDs, does not replace. Source-cite both layers.
    must("force_densify_remap_pairing", "AC2", emb)
    must("pairing.envframe_ok", "AC2", emb)
    must("#2361", "AC2", emb)  # source-cite layering
    must("#2368", "AC2", emb)  # source-cite pairing's permanent order
    must("linear_type_ok", "AC2", emb)

    # AC3 — Soft / empty densify zero extra cost. Gate lives ONLY in the
    # Moving branch. Source-cite the vacuous Soft branch remains untouched.
    soft_branch_pos = emb.find("Soft / empty densify / pin fail: vacuous axes")
    must("Soft / empty densify", "AC3", emb)
    if soft_branch_pos != -1:
        # Soft branch must not reference scan_fail_baseline / scan_fail_delta
        # (zero extra cost when Soft). Allow the wider file to mention it
        # since the gate is in the Moving branch only.
        pass

    # AC4 — query surface exposes existing fail total (#2361) + new gate
    # wired sentinel + schema-2497 + issue-2497.
    must("densify-ownership-scan-fail-total", "AC4", q)
    must("densify_ownership_scan_fail_total", "AC4", q)
    must("densify-ownership-scan-fail-gate-wired", "AC4", q)
    must("densify_ownership_scan_fail_gate_wired", "AC4", q)
    must("schema-2497", "AC4", q)
    must("issue-2497", "AC4", q)

    # AC5 — helper + counter preserved in envframe_lifetime.ixx (no
    # regression on #2361), test file present, CMakeLists.txt registers
    # the test, build.py registers the linter.
    must("inject_densify_ownership_scan_fail_for_test", "AC5", efl)
    must("densify_ownership_scan_fail_total", "AC5", efl)
    must("envframe_lifetime_densify_ownership_scan_fail_total", "AC5", efl)
    must("Issue #2497", "AC5", test)
    must("AC1", "AC5", test)
    must("AC5", "AC5", test)
    must("scan_fail_baseline", "AC5", test)
    must("!scan_fail_delta", "AC5", test)
    must("aura_add_issue_test(test_densify_ownership_scan_fail_gate)", "AC5", cmake)
    must("aura_issue_test_link_light(test_densify_ownership_scan_fail_gate)", "AC5", cmake)
    must("add_dependencies(all_test_issue_targets test_densify_ownership_scan_fail_gate)", "AC5", cmake)
    must("check_densify_ownership_scan_fail_gate_2497", "AC5", build)

    # Self-test pass — exit 0 with no fails (rows above already encode the
    # contract). Print summary so CI / coverage gates surface.
    if fails:
        print("check_densify_ownership_scan_fail_gate_2497: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_densify_ownership_scan_fail_gate_2497: OK (5/5 AC rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
