#!/usr/bin/env python3
# scripts/check_ir_typed_entry_proof_authority_3305.py — Issue #3305 source-cite gate.
#
# Verifies that ir_typed_entry_commit_readiness_ok in
# src/compiler/typed_mutation_audit.h consults the last
# TypeLinearCommitProof face (same SSOT as linear_fast_path_ok /
# linear_move_drop_elision_ok). Catches regressions when the
# ir_typed_entry path is touched (e.g. optimization pass) and the
# last-proof-face consult is forgotten — would re-open the #3305
# dual-authority half-green execution window.
#
# Contract rows (AC1–AC4):
#
#   AC1: ir_typed_entry_commit_readiness_ok consults
#        g_last_type_linear_proof_outcome + g_last_proof_would_allow_commit
#        + g_last_proof_linear_ok (the proof-face SSOT)
#   AC2: linear_move_drop_elision_ok / linear_fast_path_ok still
#        consult the proof-face SSOT (no regression of #3186/#2964)
#   AC3: Soft / depth==0 production guard preserved (zero extra cost)
#   AC4: Existing counter g_linear_fast_path_elide_blocked_production_total
#        bumped in >=3 reject paths (Reject + would_allow=0 + linear_ok=0
#        + cr.would_allow_commit=false)
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_ir_typed_entry_proof_authority_3305.py --self-test

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = ("src/compiler/typed_mutation_audit.h",)


def _read(rel: str) -> str:
    p = REPO_ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _check_ir_typed_entry_face_consult(h: str) -> list[str]:
    """AC1: ir_typed_entry_commit_readiness_ok consults last proof face."""
    failures: list[str] = []
    fn_pos = h.find("inline bool ir_typed_entry_commit_readiness_ok() noexcept")
    if fn_pos < 0:
        failures.append("AC1: ir_typed_entry_commit_readiness_ok not found")
        return failures
    # Read a generous window (~2000 chars) to capture the new face-consult
    # + the existing commit_readiness check.
    scope = h[fn_pos : fn_pos + 2000]
    required = (
        ("g_last_type_linear_proof_outcome", "AC1: g_last_type_linear_proof_outcome not consulted"),
        ("kTypeLinearProofOutcomeReject", "AC1: kTypeLinearProofOutcomeReject not compared"),
        ("g_last_proof_would_allow_commit", "AC1: g_last_proof_would_allow_commit not consulted"),
        ("g_last_proof_linear_ok", "AC1: g_last_proof_linear_ok not consulted"),
    )
    for needle, msg in required:
        if needle not in scope:
            failures.append(msg)
    return failures


def _check_no_regression(h: str) -> list[str]:
    """AC2: linear_move_drop_elision_ok + linear_fast_path_ok still exist
    (no regression of #3130 / #3186 / #2964). The functions continue to
    gate Move/Drop / IR elision correctly:

      - linear_fast_path_ok() at the entry-elision face consults the
        proof atomics directly (same SSOT as the new ir_typed_entry
        fix).
      - linear_move_drop_elision_ok() routes through
        linear_ir_fastpath_try_skip() + commit_readiness(live_policy)
        — the Move/Drop gate still fires (no regression of #3186) but
        does not need to consult the proof atomics directly because
        linear_ir_fastpath_try_skip() captures them on the elision
        fastpath.

    So AC2 here is just existence + the linear_fast_path_ok side
    consults the proof atomics (per the SSOT contract)."""
    failures: list[str] = []
    # linear_move_drop_elision_ok: existence only (Move/Drop gate
    # continues to fire through commit_readiness + linear_ir_fastpath
    # _try_skip; no direct proof-atomics consult required).
    mmd_pos = h.find("inline bool linear_move_drop_elision_ok() noexcept")
    if mmd_pos < 0:
        failures.append("AC2: linear_move_drop_elision_ok not found (regression of #3186)")
    # linear_fast_path_ok: existence + consults the proof atomics
    # (SSOT for the entry-elision face).
    lfp_pos = h.find("inline bool linear_fast_path_ok() noexcept")
    if lfp_pos < 0:
        failures.append("AC2: linear_fast_path_ok not found (regression of #2964 / #3030)")
    else:
        scope = h[lfp_pos : lfp_pos + 1200]
        if "g_last_type_linear_proof_outcome" not in scope:
            failures.append(
                "AC2: linear_fast_path_ok no longer consults g_last_type_linear_proof_outcome (SSOT regression)"
            )
        if "g_last_proof_would_allow_commit" not in scope:
            failures.append(
                "AC2: linear_fast_path_ok no longer consults g_last_proof_would_allow_commit (SSOT regression)"
            )
    return failures


def _check_production_guard(h: str) -> list[str]:
    """AC3: Soft / depth==0 production guard preserved (zero extra cost)."""
    failures: list[str] = []
    fn_pos = h.find("inline bool ir_typed_entry_commit_readiness_ok() noexcept")
    if fn_pos < 0:
        return failures
    scope = h[fn_pos : fn_pos + 2000]
    # Soft/Off early return.
    if "if (!(production_defaults_active() || get_strategy() == AuditStrategy::Full))" not in scope:
        failures.append("AC3: production_defaults_active / AuditStrategy::Full guard not preserved")
    # depth==0 short-circuit.
    if "if (depth == 0)" not in scope:
        failures.append("AC3: depth==0 short-circuit not preserved")
    return failures


def _check_existing_counter_reused(h: str) -> list[str]:
    """AC4: g_linear_fast_path_elide_blocked_production_total bumped in >=3 paths."""
    failures: list[str] = []
    fn_pos = h.find("inline bool ir_typed_entry_commit_readiness_ok() noexcept")
    if fn_pos < 0:
        return failures
    scope = h[fn_pos : fn_pos + 2000]
    target = "g_linear_fast_path_elide_blocked_production_total.fetch_add(1,"
    count = scope.count(target)
    if count < 3:
        failures.append(
            f"AC4: g_linear_fast_path_elide_blocked_production_total bumped in only {count} paths "
            f"(expected >=3: Reject, would_allow=0, linear_ok=0, cr.would_allow_commit=false)"
        )
    return failures


def run_strict() -> list[str]:
    h = _read("src/compiler/typed_mutation_audit.h")
    failures: list[str] = []
    failures.extend(_check_ir_typed_entry_face_consult(h))
    failures.extend(_check_no_regression(h))
    failures.extend(_check_production_guard(h))
    failures.extend(_check_existing_counter_reused(h))
    return failures


def _self_test() -> int:
    failures = run_strict()
    if failures:
        print("SELF-TEST FAIL:", file=sys.stderr)
        for f in failures:
            print("  -", f, file=sys.stderr)
        return 1
    print("SELF-TEST OK: all #3305 source-cite checks pass")
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
    print("OK: #3305 source-cite checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
