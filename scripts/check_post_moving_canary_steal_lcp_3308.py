#!/usr/bin/env python3
# scripts/check_post_moving_canary_steal_lcp_3308.py — Issue #3308 source-cite gate.
#
# Verifies that the post-Moving temporary/known canary race under steal×compact
# is closed by:
#   1. arena.ixx densify success path stamps the unified LifetimeConsistencyProof
#      (LCP) BEFORE clearing post_moving_live_canaries_, so steal can observe the
#      would_allow_commit state.
#   2. evaluator_fiber_mutation.cpp steal_complete path re-consults the LCP
#      atomics and refuses/soft-degrades publish if the last densify had
#      objects_moved > 0 AND the LCP says deny (stale canary window).
#
# Catches regressions when either side is touched (e.g. refactor moves the
# canary clear before the LCP stamp, or steal_complete skips the re-consult)
# — would re-open the canary race window between densify rewrite and canary
# clear that the issue closes.
#
# Contract rows (AC1–AC6 from the test file):
#
#   AC1: arena.ixx densify success path calls
#        stamp_lifetime_consistency_proof(...) BEFORE post_moving_live_canaries_.clear()
#   AC2: includes "core/lifetime_consistency_proof.hh" added in arena.ixx
#   AC3: LCP proof.would_allow_commit reflects stale canary / incomplete state
#        (set to false when result.moving_incomplete_remap ||
#         !result.pin_contract_held || result.untracked_kept_count > 0 ||
#         result.post_moving_stale_count > 0)
#   AC4: evaluator_fiber_mutation.cpp steal_complete path re-consults LCP
#        atomics (g_lcp_last_would_allow_commit + g_lcp_last_mutation_epoch)
#        BEFORE the new LCP stamp at line 2629+ (Issue #2888)
#   AC5: refuse/soft-degrade when last densify had objects_moved > 0
#        (g_lcp_last_mutation_epoch() != 0) AND LCP says deny
#        (g_lcp_last_would_allow_commit() == 0)
#   AC6: no docs/design/3308-*; no test_issue_3308.cpp (per #1655/#81934);
#        extend existing test_moving_densify_fail_closed (per #81967)
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_post_moving_canary_steal_lcp_3308.py --self-test

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = (
    "src/core/arena.ixx",
    "src/compiler/evaluator_fiber_mutation.cpp",
    "tests/core/test_moving_densify_fail_closed.cpp",
)


def _read(rel: str) -> str:
    p = REPO_ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _check_densify_lcp_stamp(arena: str) -> list[str]:
    """AC1: arena.ixx densify success path stamps LCP BEFORE canary clear."""
    failures: list[str] = []
    # Anchor on the post_moving_live_canaries_.clear() line.
    clear_pos = arena.find("post_moving_live_canaries_.clear();")
    if clear_pos < 0:
        failures.append("AC1: post_moving_live_canaries_.clear() not found in arena.ixx")
        return failures
    # Walk back 4000 chars to find the stamp call before the clear.
    scope_start = max(0, clear_pos - 4000)
    scope = arena[scope_start:clear_pos]
    if "stamp_lifetime_consistency_proof(" not in scope:
        failures.append(
            "AC1: stamp_lifetime_consistency_proof(...) NOT called BEFORE "
            "post_moving_live_canaries_.clear() — canary race window re-opens"
        )
    return failures


def _check_lcp_include(arena: str) -> list[str]:
    """AC2: arena.ixx includes core/lifetime_consistency_proof.hh."""
    failures: list[str] = []
    if '#include "core/lifetime_consistency_proof.hh"' not in arena:
        failures.append('AC2: arena.ixx must include "core/lifetime_consistency_proof.hh" for the LCP stamp call')
    return failures


def _check_proof_would_allow_commit_reflects_state(arena: str) -> list[str]:
    """AC3: LCP proof.would_allow_commit reflects incomplete state."""
    failures: list[str] = []
    # Anchor on the stamp call.
    stamp_pos = arena.find("stamp_lifetime_consistency_proof(")
    if stamp_pos < 0:
        failures.append("AC3: stamp_lifetime_consistency_proof call not found")
        return failures
    scope_start = max(0, stamp_pos - 1500)
    scope = arena[scope_start:stamp_pos]
    if "moving_incomplete_remap" not in scope:
        failures.append("AC3: proof.would_allow_commit must check result.moving_incomplete_remap")
    if "pin_contract_held" not in scope:
        failures.append("AC3: proof.would_allow_commit must check result.pin_contract_held")
    if "untracked_kept_count" not in scope:
        failures.append("AC3: proof.would_allow_commit must check result.untracked_kept_count")
    if "post_moving_stale_count" not in scope:
        failures.append("AC3: proof.would_allow_commit must check result.post_moving_stale_count")
    return failures


def _check_steal_complete_reconsult(fiber: str) -> list[str]:
    """AC4: evaluator_fiber_mutation.cpp steal_complete re-consults LCP BEFORE new stamp."""
    failures: list[str] = []
    # The fix may live either in the strong def aura_evaluator_on_steal_complete
    # or in a helper called from it (e.g. the Issue #2888 LCP stamp helper at
    # line ~2629). The check is "the strings exist in the file" + "the re-consult
    # comes BEFORE the new LCP stamp". Scan the whole file.
    if "g_lcp_last_would_allow_commit" not in fiber:
        failures.append(
            "AC4: steal_complete must re-consult g_lcp_last_would_allow_commit() (Issue #3308: canary race window)"
        )
    if "g_lcp_last_mutation_epoch" not in fiber:
        failures.append(
            "AC4: steal_complete must re-consult g_lcp_last_mutation_epoch() "
            "(tracks last densify had objects_moved > 0)"
        )
    # The re-consult must come BEFORE the existing LCP stamp helper
    # (which is at the comment "Issue #2888: stamp unified LifetimeConsistencyProof").
    reconsult_pos = fiber.find("g_lcp_last_would_allow_commit")
    stamp2888_pos = fiber.find("Issue #2888: stamp unified LifetimeConsistencyProof")
    if reconsult_pos > 0 and stamp2888_pos > 0 and reconsult_pos > stamp2888_pos:
        failures.append(
            "AC4: re-consult must come BEFORE the Issue #2888 stamp helper (otherwise the densify state is overwritten)"
        )
    return failures


def _check_refuse_under_densify_incomplete(fiber: str) -> list[str]:
    """AC5: steal_complete refuses/soft-degrades when last densify had objects_moved > 0 AND LCP says deny."""
    failures: list[str] = []
    # Same scope as AC4 — whole file (fix may be in helper or strong def).
    if "last_epoch != 0 && !last_would_allow" not in fiber:
        failures.append(
            "AC5: steal_complete must check 'last_epoch != 0 && !last_would_allow' "
            "(last densify had objects_moved > 0 AND LCP says deny → refuse)"
        )
    if "g_moving_post_moving_stale_total.fetch_add" not in fiber:
        failures.append("AC5: steal_complete must bump g_moving_post_moving_stale_total on refuse/soft-degrade")
    return failures


def _check_no_issue_numbered_artifacts() -> list[str]:
    """AC6: no docs/design/3308-*; no test_issue_3308.cpp."""
    failures: list[str] = []
    docs = REPO_ROOT / "docs" / "design"
    if docs.exists():
        for f in docs.iterdir():
            if "3308-" in f.name:
                failures.append(f"AC6: forbidden {f.name} (per #1655)")
    for rel in (
        "tests/issues/test_issue_3308.cpp",
        "tests/core/test_issue_3308.cpp",
        "tests/serve/test_issue_3308.cpp",
    ):
        p = REPO_ROOT / rel
        if p.exists():
            failures.append(f"AC6: forbidden {rel} (per #81934)")
    # Linter script presence (registered in build.py).
    if not (REPO_ROOT / "scripts/check_post_moving_canary_steal_lcp_3308.py").exists():
        failures.append("AC6: source-cite linter script present")
    return failures


def run_strict() -> list[str]:
    arena = _read("src/core/arena.ixx")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    failures: list[str] = []
    failures.extend(_check_densify_lcp_stamp(arena))
    failures.extend(_check_lcp_include(arena))
    failures.extend(_check_proof_would_allow_commit_reflects_state(arena))
    failures.extend(_check_steal_complete_reconsult(fiber))
    failures.extend(_check_refuse_under_densify_incomplete(fiber))
    failures.extend(_check_no_issue_numbered_artifacts())
    return failures


def _self_test() -> int:
    failures = run_strict()
    if failures:
        print("SELF-TEST FAIL:", file=sys.stderr)
        for f in failures:
            print("  -", f, file=sys.stderr)
        return 1
    print("SELF-TEST OK: all #3308 source-cite checks pass")
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
    print("OK: #3308 source-cite checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
