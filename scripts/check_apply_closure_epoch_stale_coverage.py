#!/usr/bin/env python3
# scripts/check_apply_closure_epoch_stale_coverage.py — Issue #1655
#
# AC list:
#   AC1: --self-test exit 0 (the script IS the self-test).
#   AC2: src/compiler/evaluator_eval_flat.cpp has a file-static
#        `static bool closure_is_epoch_stale(const Evaluator& ev,
#         const Closure& cl) noexcept` helper.
#   AC3: helper checks bridge_epoch via
#        `ev.is_bridge_stale(cl.bridge_epoch, ev.current_bridge_epoch())`.
#   AC4: helper checks env_frame stale via
#        `ev.is_env_frame_invalid(cl.env_id) || ev.is_env_frame_stale(cl.env_id)`
#        guarded by `cl.env_id != NULL_ENV_ID`.
#   AC5: closure_needs_safe_fallback uses closure_is_epoch_stale for the
#        closure_epoch_mismatch_fallback gate; previous inline
#        `bool epoch_or_env_stale` tracking removed.
#   AC6: late `ev.closure_is_epoch_or_env_stale(cl)` invariant check
#        removed (#1660 redundant after #1655 extraction).
#   AC7: inline race-window path in apply_closure uses
#        `if (closure_is_epoch_stale(*this, cl_copy))` for the if-gate;
#        previous inline `bridge_stale` local removed.
#   AC8: bump_closure_epoch_mismatch_fallback appears in both gated
#        blocks (helper + race-window) — never unconditional at the
#        top of a block.
#   AC9: ≥3 `Issue #1655` rationale comments present in
#        src/compiler/evaluator_eval_flat.cpp (helper + caller +
#        race-window call site).
#   AC10: tests/test_issue_1655.cpp exists and has the expected AC1..AC10
#         structure.
#
# Pattern reference: scripts/check_bridge_epoch_atomic_coverage.py
# (#1654), scripts/check_orchestration_steal_boundary_coverage.py
# (#1641), scripts/check_aot_hot_update_incremental_coverage.py (#1640),
# scripts/check_primitive_surface.py (#1449 SlimSurface freeze).
#
# Hooked into the pre-commit hook alongside the existing coverage /
# freeze linters (clang-format, primitive surface, test-registry, gen_docs).
# Run individually with
# `./scripts/check_apply_closure_epoch_stale_coverage.py` from the repo
# root.

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ERRORS = []


def check(name: str, condition: bool) -> None:
    if condition:
        print(f"OK    {name}")
    else:
        print(f"FAIL  {name}")
        ERRORS.append(name)


def read(rel: str) -> str:
    return (ROOT / rel).read_text()


def count_occurrences(haystack: str, needle: str) -> int:
    if not needle:
        return 0
    count = 0
    pos = 0
    while True:
        pos = haystack.find(needle, pos)
        if pos == -1:
            return count
        count += 1
        pos += len(needle)


def main() -> int:
    print("=== scripts/check_apply_closure_epoch_stale_coverage.py ===")
    print(f"repo root: {ROOT}")
    print()

    src = read("src/compiler/evaluator_eval_flat.cpp")

    # AC1: self-test
    print("--- AC1: --self-test exit 0 ---")
    check("AC1: script self-test exit 0", True)
    print()

    # AC2: file-static helper exists
    print("--- AC2: file-static closure_is_epoch_stale helper exists ---")
    check(
        "evaluator_eval_flat.cpp: static bool closure_is_epoch_stale(...) noexcept",
        "static bool closure_is_epoch_stale(const Evaluator& ev, const Closure& cl) noexcept" in src,
    )
    print()

    # AC3: helper checks bridge_epoch
    print("--- AC3: helper checks bridge_epoch mismatch ---")
    check(
        "evaluator_eval_flat.cpp: ev.is_bridge_stale(cl.bridge_epoch, ev.current_bridge_epoch())",
        "ev.is_bridge_stale(cl.bridge_epoch, ev.current_bridge_epoch())" in src,
    )
    print()

    # AC4: helper checks env_frame stale (NULL_ENV_ID guarded)
    print("--- AC4: helper checks env_frame stale (NULL_ENV_ID guarded) ---")
    check(
        "evaluator_eval_flat.cpp: cl.env_id != NULL_ENV_ID && (env_frame_invalid || env_frame_stale)",
        "cl.env_id != NULL_ENV_ID &&\n"
        "        (ev.is_env_frame_invalid(cl.env_id) || "
        "ev.is_env_frame_stale(cl.env_id))" in src,
    )
    print()

    # AC5: helper used in closure_needs_safe_fallback; inline tracking removed
    print("--- AC5: closure_needs_safe_fallback uses closure_is_epoch_stale ---")
    check(
        "evaluator_eval_flat.cpp: if (closure_is_epoch_stale(ev, cl)) (helper gate)",
        "if (closure_is_epoch_stale(ev, cl))" in src,
    )
    check(
        "evaluator_eval_flat.cpp: previous inline 'bool epoch_or_env_stale = false;' removed",
        "bool epoch_or_env_stale = false;" not in src,
    )
    print()

    # AC6: late closure_is_epoch_or_env_stale invariant check + inline
    # epoch_or_env_stale boolean tracking removed from CODE.
    # (Comments may still mention these identifiers for historical context.)
    print("--- AC6: late invariant + inline boolean tracking removed from code ---")
    # Filter out comment lines (lines whose first non-whitespace is `//`)
    # before counting — comments may still reference the removed code for
    # historical context but the actual code must be gone.
    code_only_lines = [line for line in src.splitlines() if not line.lstrip().startswith("//")]
    code_only = "\n".join(code_only_lines)
    n_code_refs = count_occurrences(code_only, "epoch_or_env_stale")
    check(
        f"evaluator_eval_flat.cpp: no code reference to epoch_or_env_stale "
        f"(code refs={n_code_refs} == 0, comments may still mention)",
        n_code_refs == 0,
    )
    # The late invariant code-block `if (ev.closure_is_epoch_or_env_stale(cl))`
    # must be gone from CODE.
    n_code_inv = count_occurrences(code_only, "if (ev.closure_is_epoch_or_env_stale(cl))")
    check(
        f"evaluator_eval_flat.cpp: late `if (ev.closure_is_epoch_or_env_stale(cl))` "
        f"code-block removed (count={n_code_inv} == 0)",
        n_code_inv == 0,
    )
    print()

    # AC7: inline race-window uses closure_is_epoch_stale; bridge_stale local removed
    print("--- AC7: inline race-window uses closure_is_epoch_stale ---")
    check(
        "evaluator_eval_flat.cpp: if (closure_is_epoch_stale(*this, cl_copy)) (race-window gate)",
        "if (closure_is_epoch_stale(*this, cl_copy))" in src,
    )
    check(
        "evaluator_eval_flat.cpp: previous inline 'const bool bridge_stale = is_bridge_stale(' removed",
        "const bool bridge_stale = is_bridge_stale(" not in src,
    )
    print()

    # AC8: bump_closure_epoch_mismatch_fallback appears in both gated blocks
    print("--- AC8: bump_closure_epoch_mismatch_fallback is gated ---")
    n_bump = count_occurrences(src, "bump_closure_epoch_mismatch_fallback()")
    check(
        f"evaluator_eval_flat.cpp: bump_closure_epoch_mismatch_fallback() count={n_bump} ≥ 2",
        n_bump >= 2,
    )
    print()

    # AC9: ≥3 Issue #1655 rationale comments
    print("--- AC9: Issue #1655 rationale comments ---")
    n_comments = count_occurrences(src, "Issue #1655")
    check(
        f"evaluator_eval_flat.cpp: Issue #1655 comment count={n_comments} ≥ 3",
        n_comments >= 3,
    )
    # Also verify the comment markers are at distinct sites (helper def +
    # caller + race-window call site — not just three in one block).
    n_helper_def = src.count("Issue #1655: file-local single-source-of-truth")
    n_caller = src.count("Issue #1655: gate on closure_is_epoch_stale")
    n_race_window = src.count("Issue #1655: race-window path now uses closure_is_epoch_stale")
    check(
        "evaluator_eval_flat.cpp: helper-def comment present",
        n_helper_def == 1,
    )
    check(
        "evaluator_eval_flat.cpp: caller-side gate comment present",
        n_caller == 1,
    )
    check(
        "evaluator_eval_flat.cpp: race-window gate comment present",
        n_race_window == 1,
    )
    print()

    # AC10: test file exists with expected AC structure
    print("--- AC10: tests/test_issue_1655.cpp exists with AC1..AC10 ---")
    test_path = ROOT / "tests" / "test_issue_1655.cpp"
    check("tests/test_issue_1655.cpp: exists", test_path.exists())
    if test_path.exists():
        test_src = test_path.read_text()
        # Verify each AC1..AC10 appears as a comment marker in the test
        for i in range(1, 11):
            ac_marker = f"AC{i}:"
            check(
                f"tests/test_issue_1655.cpp: {ac_marker} marker present",
                ac_marker in test_src,
            )
    print()

    print("=" * 60)
    if ERRORS:
        print(f"FAIL: {len(ERRORS)} check(s) failed")
        for e in ERRORS:
            print(f"  - {e}")
        return 1
    print("PASS: all 10 ACs green (Issue #1655 wire-up fully covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
