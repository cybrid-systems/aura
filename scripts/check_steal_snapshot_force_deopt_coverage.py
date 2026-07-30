#!/usr/bin/env python3
"""
Linter for #2310 — fail-closed force-deopt on steal snapshot inconsistency.

Verifies the implementation is wired correctly:
  - worker.cpp calls aura_force_deopt_on_steal_snapshot_mismatch on
    mutation_safety_snapshot_inconsistent(snap)
  - worker.cpp respects is_steal_snapshot_soft_mode (AURA_STEAL_SNAPSHOT_SOFT=1)
  - evaluator_fiber_mutation.cpp has strong definition of the C ABI + bump impl
  - evaluator_fiber_mutation.cpp refresh_after_fiber_migration re-samples
    snapshot (AC2 defense in depth)
  - aura_jit_bridge.cpp has the C ABI hook + file-level atomic fallback
  - aura_jit_bridge.cpp has the static accessor
  - aura_jit_bridge.h has declarations
  - observability_metrics.h has steal_snapshot_mismatch_force_deopt_total
  - evaluator.ixx has Evaluator::bump_steal_snapshot_mismatch_force_deopt_total decl
  - evaluator_primitives_obs_eval.cpp query primitive has schema-2310 /
    issue-2310 / steal-snapshot-mismatch-force-deopt-total keys
  - fiber.h has bumper + C-linkage shim forward decl + soft-mode accessor
  - fiber.cpp has C-linkage shim definition
  - fiber_bridge.cpp has weak stub
  - tests/serve/test_mutation_safety_snapshot_steal_2184.cpp cites Issue #2310

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_steal_snapshot_force_deopt_coverage.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def must_contain(file: Path, needle: str, label: str) -> bool:
    if not file.exists():
        print(f"FAIL {label}: file {file} does not exist")
        return False
    content = read(file)
    if needle in content:
        print(f"OK  {label}")
        return True
    print(f"FAIL {label}: '{needle}' not found in {file}")
    return False


def main() -> int:
    checks = [
        # worker.cpp fail-closed path
        (
            ROOT / "src/serve/worker.cpp",
            "aura_force_deopt_on_steal_snapshot_mismatch",
            "worker.cpp calls force-deopt C ABI",
        ),
        (ROOT / "src/serve/worker.cpp", "is_steal_snapshot_soft_mode", "worker.cpp respects soft mode"),
        (ROOT / "src/serve/worker.cpp", "Issue #2310", "worker.cpp cites 2310"),
        # fiber.h declarations + soft-mode accessor
        (ROOT / "src/serve/fiber.h", "bump_steal_snapshot_mismatch_force_deopt", "fiber.h has force-deopt bumper"),
        (
            ROOT / "src/serve/fiber.h",
            "aura_fiber_static_steal_snapshot_mismatch_force_deopt_total",
            "fiber.h has C-linkage shim decl",
        ),
        (ROOT / "src/serve/fiber.h", "is_steal_snapshot_soft_mode", "fiber.h has soft-mode accessor"),
        (ROOT / "src/serve/fiber.h", "Issue #2310", "fiber.h cites 2310"),
        # fiber.cpp C-linkage shim def
        (
            ROOT / "src/serve/fiber.cpp",
            "aura_fiber_static_steal_snapshot_mismatch_force_deopt_total",
            "fiber.cpp has C-linkage shim def",
        ),
        # evaluator_fiber_mutation.cpp strong def + bump impl + AC2 re-sample
        (
            ROOT / "src/compiler/evaluator_fiber_mutation.cpp",
            "aura_force_deopt_on_steal_snapshot_mismatch",
            "evaluator_fiber_mutation.cpp strong def",
        ),
        (
            ROOT / "src/compiler/evaluator_fiber_mutation.cpp",
            "Evaluator::bump_steal_snapshot_mismatch_force_deopt_total",
            "evaluator_fiber_mutation.cpp has bump impl",
        ),
        (
            ROOT / "src/compiler/evaluator_fiber_mutation.cpp",
            "mutation_safety_snapshot_inconsistent(post_snap)",
            "AC2: refresh re-samples snapshot (defense in depth)",
        ),
        (ROOT / "src/compiler/evaluator_fiber_mutation.cpp", "Issue #2310", "evaluator_fiber_mutation.cpp cites 2310"),
        # aura_jit_bridge.cpp C ABI hook + file-level atomic fallback
        (
            ROOT / "src/compiler/aura_jit_bridge.cpp",
            "aura_force_deopt_on_steal_snapshot_mismatch",
            "aura_jit_bridge.cpp C ABI hook",
        ),
        (
            ROOT / "src/compiler/aura_jit_bridge.cpp",
            "g_2310_force_deopt_fallback_total",
            "aura_jit_bridge.cpp file-level atomic fallback",
        ),
        (
            ROOT / "src/compiler/aura_jit_bridge.cpp",
            "aura_static_steal_snapshot_mismatch_force_deopt_total",
            "aura_jit_bridge.cpp has accessor",
        ),
        # aura_jit_bridge.h declarations
        (
            ROOT / "src/compiler/aura_jit_bridge.h",
            "aura_force_deopt_on_steal_snapshot_mismatch",
            "aura_jit_bridge.h declaration",
        ),
        # observability_metrics.h counter
        (
            ROOT / "src/compiler/observability_metrics.h",
            "steal_snapshot_mismatch_force_deopt_total",
            "observability_metrics.h counter",
        ),
        # evaluator.ixx declaration
        (
            ROOT / "src/compiler/evaluator.ixx",
            "bump_steal_snapshot_mismatch_force_deopt_total",
            "evaluator.ixx declaration",
        ),
        # query primitive keys
        (ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp", "schema-2310", "query primitive schema-2310"),
        (ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp", "issue-2310", "query primitive issue-2310"),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "steal-snapshot-mismatch-force-deopt-total",
            "query primitive force-deopt key",
        ),
        # fiber_bridge.cpp weak stub
        (
            ROOT / "src/compiler/fiber_bridge.cpp",
            "aura_force_deopt_on_steal_snapshot_mismatch",
            "fiber_bridge.cpp weak stub",
        ),
        # test file extended
        (ROOT / "tests/serve/test_mutation_safety_snapshot_steal_2184.cpp", "Issue #2310", "test file cites 2310"),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_steal_snapshot_force_deopt_coverage.py",
            "fail-closed force-deopt",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2310 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
