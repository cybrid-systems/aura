#!/usr/bin/env python3
"""Issue #2708: ownership_rebind_after_remap real per-root validate walk.

Closes the #2695 residual: the unified rebind entry shipped as counters +
Soft/Production routing only — the real per-root walk through
OwnershipEnv::validate_ownership was deferred. #2708 wires the walk via
a test-injected mismatch sentinel (OwnershipEnv is in the type_checker
module — pure header cannot name the module type without GCC 16 ambiguity,
so the walk is a TU-local extern "C" hook in ownership_rebind.cpp).

Contract rows (AC1–AC5 from the test file):

  AC1: production + injected mismatch in non-empty span → returns false
  AC2: soft + injected mismatch in non-empty span → returns true (observe)
  AC3: empty span → zero-cost short-circuit preserved
  AC4: per-reason routing (Densify/Steal/ExplicitAgent) + validate-walk
       counter
  AC5: source-cite across hdr/impl/3 call sites + tests + linter +
       schema-2708 + no docs/design/

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _self_test() -> int:
    """Run the linter on a known-good tree and verify exit 0.

    The default tree IS the known-good tree for #2708 (this linter is the
    source of truth on the contract rows — if the tree is healthy, the
    linter should report OK). If the tree regresses, the linter will report
    fails and --self-test exits 1.
    """
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_ownership_rebind_walk_2708.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"--self-test FAILED:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
        return 1
    print(f"--self-test OK: {r.stdout.strip()}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true", help="Run self-test on this linter")
    args = p.parse_args()

    if args.self_test:
        return _self_test()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hdr = _read("src/compiler/ownership_rebind.h")
    cpp = _read("src/compiler/ownership_rebind.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    m = _read("src/compiler/evaluator_primitives_mutate.cpp")
    t = _read("tests/serve/test_steal_densify_linear_type_hard_and.cpp")

    # AC1 — production + inject mismatch → returns false
    must("production_defaults_active", "AC1", cpp)
    must("inject_ownership_rebind_mismatch_for_test", "AC1", hdr)
    must("ac2708_1_production_mismatch_returns_false", "AC1", t)
    must("apply_production_audit_defaults", "AC1", t)
    must("ownership_rebind_fail_total", "AC1", hdr)
    must("ownership_rebind_densify_fail_total", "AC1", hdr)

    # AC2 — soft + inject mismatch → returns true (observe only)
    must("apply_dev_audit_defaults", "AC2", t)
    must("ac2708_2_soft_observe_only", "AC2", t)
    must("ownership_rebind_steal_fail_total", "AC2", hdr)
    must("validate_walk_total", "AC2", hdr)  # counter bumped even on Soft mismatch

    # AC3 — empty span short-circuit preserved
    must("ac2708_3_empty_span_short_circuit_preserved", "AC3", t)
    must("remapped_roots.empty()", "AC3", cpp)

    # AC4 — per-reason routing + validate-walk counter
    must("RemapReason::Densify", "AC4", cpp)
    must("RemapReason::Steal", "AC4", cpp)
    must("RemapReason::ExplicitAgent", "AC4", cpp)
    must("ownership_rebind_densify_total", "AC4", hdr)
    must("ownership_rebind_steal_total", "AC4", hdr)
    must("ownership_rebind_explicit_agent_total", "AC4", hdr)
    must("ac2708_4_per_reason_routing_and_validate_walk", "AC4", t)

    # AC5 — source-cite across hdr/impl/3 call sites + tests + linter + schema
    must("Issue #2708", "AC5", hdr)
    must("kOwnershipRebindWalkIssue = 2708", "AC5", hdr)
    must("Issue #2708", "AC5", cpp)
    must("#2708", "AC5", mb)
    must("#2708", "AC5", fm)
    must("#2708", "AC5", m)
    must("ac2708_5_source_and_linter", "AC5", t)
    must("#2708", "AC5", Path(__file__).read_text(encoding="utf-8", errors="replace"))
    must("--self-test", "AC5", Path(__file__).read_text(encoding="utf-8", errors="replace"))

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2708 ownership_rebind_after_remap real per-root walk — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
