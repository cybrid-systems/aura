#!/usr/bin/env python3
# scripts/check_pending_full_solve_residual_hardlatch_3307.py — Issue #3307 source-cite gate.
#
# Verifies that the production budget-allow path in
# src/compiler/type_checker_impl.cpp escalate_locality_slo_if_production
# hard-latches the pending-full-solve residual face via
# note_pending_full_solve_residual(residual, /*hard=*/true). Without
# this, mid-batch IR / Agent poll observes SOLVED face + empty residual
# face while CS still has dirty + pending roots between #2994 handoff
# and #3190/#3031 drain.
#
# Contract rows (AC1–AC5 from the test file):
#
#   AC1: budget-allow path under if (hard) calls
#        note_pending_full_solve_residual(residual, /*hard=*/true)
#   AC2: Soft path (if !hard) does NOT call note_pending_full_solve_residual
#        (observe-only via existing #2994 contract)
#   AC3: quiet path (residual == 0) does NOT call
#        note_pending_full_solve_residual (zero extra atomics)
#   AC4: drain_pending_full_solve_before_commit clears face via
#        note_pending_full_solve_residual(0, true) on SOLVED
#   AC5: commit_readiness_live_policy still reads
#        pending_full_solve_residual_face_hit() (no new query key)
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_pending_full_solve_residual_hardlatch_3307.py --self-test

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/type_checker_impl.cpp",
    "src/compiler/typed_mutation_audit.h",
)


def _read(rel: str) -> str:
    p = REPO_ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _check_budget_allow_hard_latch(impl: str) -> list[str]:
    """AC1: budget-allow path under if (hard) calls note_pending_full_solve_residual."""
    failures: list[str] = []
    anchor = "if (budget > 0 && residual > 0 && residual <= static_cast<std::size_t>(budget)) {"
    pos = impl.find(anchor)
    if pos < 0:
        failures.append("AC1: budget-allow branch not found in escalate_locality_slo_if_production")
        return failures
    scope = impl[pos : pos + 2500]
    if "Issue #3307: hard-latch the pending-full-solve residual face" not in scope:
        failures.append("AC1: comment documents the hard-latch rationale")
    if "aura::compiler::typed_audit::note_pending_full_solve_residual(" not in scope:
        failures.append("AC1: note_pending_full_solve_residual called in budget-allow path")
    if "/*hard=*/true" not in scope:
        failures.append("AC1: hard=true flag (face=1) used")
    if "if (hard)" not in scope:
        failures.append("AC1: gated on production / Full (if (hard))")
    if "return prior;" not in scope:
        failures.append("AC1: still returns SOLVED (prior) — handoff face is the gate")
    return failures


def _check_soft_observe_only(impl: str) -> list[str]:
    """AC2: Soft path does NOT call note_pending_full_solve_residual."""
    failures: list[str] = []
    pos = impl.find("if (!hard) {")
    if pos < 0:
        failures.append("AC2: Soft branch not found in escalate_locality_slo_if_production")
        return failures
    scope = impl[pos : pos + 800]
    if "solve_delta_locality_slo_observe_total" not in scope:
        failures.append("AC2: Soft path bumps observe counter (existing #2994 contract)")
    if "note_pending_full_solve_residual(" in scope:
        failures.append("AC2: Soft path must NOT call note_pending_full_solve_residual (no hard latch)")
    if "return prior;" not in scope:
        failures.append("AC2: Soft path still returns SOLVED (prior) without hard latch")
    return failures


def _check_quiet_zero_cost(impl: str) -> list[str]:
    """AC3: quiet residual==0 path does NOT call note_pending_full_solve_residual."""
    failures: list[str] = []
    # Anchor on the function DEFINITION (ConstraintSystem::escalate_locality_slo_if_production)
    # — `find("escalate_locality_slo_if_production")` would also match call sites
    # which have shorter surrounding context.
    pos = impl.find("ConstraintSystem::escalate_locality_slo_if_production(SolveResult prior,")
    if pos < 0:
        failures.append("AC3: escalate_locality_slo_if_production definition not found")
        return failures
    scope = impl[pos : pos + 1500]
    if "if (prior != SolveResult::SOLVED)" not in scope:
        failures.append("AC3: quiet early-return guard on prior != SOLVED")
    if "last_locality_pruned_ == 0 && dirty_count_ == 0" not in scope:
        failures.append("AC3: quiet early-return guard on zero residual / dirty")
    early_return = scope.find("return prior;")
    if early_return >= 0:
        quiet_scope = scope[:early_return]
        if "note_pending_full_solve_residual(" in quiet_scope:
            failures.append("AC3: quiet path must not call note_pending_full_solve_residual")
    return failures


def _check_drain_clears_face(impl: str) -> list[str]:
    """AC4: drain_pending_full_solve_before_commit clears face on SOLVED."""
    failures: list[str] = []
    pos = impl.find("ConstraintSystem::drain_pending_full_solve_before_commit(std::vector<Constraint>* unresolved_out)")
    if pos < 0:
        failures.append("AC4: drain_pending_full_solve_before_commit not found")
        return failures
    scope = impl[pos : pos + 2500]
    if "note_pending_full_solve_residual(0, true)" not in scope:
        failures.append("AC4: drain clears face via note_pending_full_solve_residual(0, true)")
    return failures


def _check_existing_surfaces(ixx: str) -> list[str]:
    """AC5: commit_readiness_live_policy still reads pending_full_solve_residual_face_hit()."""
    failures: list[str] = []
    # Anchor on the function DEFINITION (the constexpr/[[nodiscard]] declaration line)
    # — `find("commit_readiness_live_policy()")` would match the call site first
    # (e.g. `ir_typed_entry_commit_readiness_ok` in #3305 reads the live policy)
    # which is shorter and lacks the body that calls pending_full_solve_residual_face_hit().
    pos = ixx.find("[[nodiscard]] CommitReadinessInput commit_readiness_live_policy() noexcept")
    if pos < 0:
        # Fallback for declarations without nodiscard (some header-only functions).
        pos = ixx.find("CommitReadinessInput commit_readiness_live_policy() noexcept {")
    if pos < 0:
        failures.append("AC5: commit_readiness_live_policy definition not found")
        return failures
    scope = ixx[pos : pos + 2500]
    if "pending_full_solve_residual_face_hit()" not in scope:
        failures.append(
            "AC5: commit_readiness_live_policy still reads pending_full_solve_residual_face_hit() (no new query key)"
        )
    if "pending_full_solve_residual_face" not in ixx:
        failures.append("AC5: existing pending_full_solve_residual_face atomics reused (no second model)")
    return failures


def run_strict() -> list[str]:
    impl = _read("src/compiler/type_checker_impl.cpp")
    ixx = _read("src/compiler/typed_mutation_audit.h")
    failures: list[str] = []
    failures.extend(_check_budget_allow_hard_latch(impl))
    failures.extend(_check_soft_observe_only(impl))
    failures.extend(_check_quiet_zero_cost(impl))
    failures.extend(_check_drain_clears_face(impl))
    failures.extend(_check_existing_surfaces(ixx))
    return failures


def _self_test() -> int:
    failures = run_strict()
    if failures:
        print("SELF-TEST FAIL:", file=sys.stderr)
        for f in failures:
            print("  -", f, file=sys.stderr)
        return 1
    print("SELF-TEST OK: all #3307 source-cite checks pass")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else "")
    ap.add_argument(
        "--self-test", action="store_true", help="Run the linter against the current repo; expect zero failures."
    )
    ap.add_argument(
        "--strict", action="store_true", default=True, help="Default mode: emit failures and exit non-zero on any."
    )
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures = run_strict()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: #3307 source-cite checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
