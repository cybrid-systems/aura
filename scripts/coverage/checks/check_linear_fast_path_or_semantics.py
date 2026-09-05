#!/usr/bin/env python3
# check_linear_fast_path_or_semantics.py -- Issue #3558 OR-semantics linter
#
# Refuses any of the three production sites that use the old max-style pick
# (`override >= 0 ? override : depth`) for the linear fast-path gate.
# Either signal alone must block elision.
#
# Sites:
#   1. src/compiler/typed_mutation_audit.h
#      - linear_fast_path_ok() mid-boundary arm
#      - linear_ir_fastpath_try_skip() #3238 re-sample block
#   2. src/compiler/typed_mutation_audit_hooks.cpp
#      - aura_linear_fast_path_depth_or_densify_block() (the "or" name
#        must match behavior)
#
# Pattern detection: locate `g_linear_ir_fastpath_boundary_depth_override`
# inside one of the known function bodies, then look for the old
# `depth = static_cast<std::size_t>(g_linear_ir_fastpath_boundary_depth_override)`
# ternary pick within a window after the function name. The presence of
# this pick is the failure mode — the absence plus the presence of two
# independent `> 0` branches is the OR-semantics pattern we want.
#
# --strict: exit 1 on any violation, 0 on clean.
# --self-test: runs an inline AC matrix (override=0 + actual depth>0 must
# block) and exits 1 on any failed case.

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

AUDIT_H = REPO_ROOT / "src" / "compiler" / "typed_mutation_audit.h"
HOOKS_CPP = REPO_ROOT / "src" / "compiler" / "typed_mutation_audit_hooks.cpp"

# Functions that must use OR semantics (override > 0 || actual depth > 0).
# Each tuple: (file, function name, OR-semantics marker).
# The OR marker is a snippet that proves both signals are checked
# independently. If found, the site passes.
SITES = [
    (
        AUDIT_H,
        "[[nodiscard]] inline bool linear_fast_path_ok()",
        [
            # OR-semantics mid-boundary arm: two independent branches, one
            # branch each (Soft/Off quiet path).
            "if (g_linear_ir_fastpath_boundary_depth_override > 0)\n            return false;",
            "if (aura_evaluator_mutation_boundary_depth() > 0)\n            return false;",
        ],
    ),
    (
        AUDIT_H,
        "[[nodiscard]] inline bool linear_ir_fastpath_try_skip()",
        [
            # Combined predicate in the #3238 re-sample block.
            "if (g_linear_ir_fastpath_boundary_depth_override > 0 ||\n"
            "            aura_evaluator_mutation_boundary_depth() > 0 || pending > 0)",
        ],
    ),
    (
        HOOKS_CPP,
        'extern "C" int aura_linear_fast_path_depth_or_densify_block',
        [
            # OR-semantics: two early-returns, either signal blocks.
            "if (g_linear_ir_fastpath_boundary_depth_override > 0)\n        return 1;",
            "if (aura_evaluator_mutation_boundary_depth() > 0)\n        return 1;",
        ],
    ),
]

# Window size after the function definition to scan (the function bodies
# for these helpers are small — 2500 chars is generous for the audit.h
# linear_fast_path_ok body).
WINDOW = 2500

# The old max-style pick signature: depth := override ?: depth
MAX_PICK_SNIPPET = "depth = static_cast<std::size_t>(g_linear_ir_fastpath_boundary_depth_override)"


def find_function_body(src: str, fn_name: str) -> str:
    """Return up to WINDOW chars of source following the *body* definition
    of `fn_name`. For hooks.cpp the second definition (extern "C" impl) is
    the one with the runtime behavior; for audit.h we want the body, not
    any forward declaration."""
    occurrences = []
    pos = 0
    while True:
        idx = src.find(fn_name, pos)
        if idx == -1:
            break
        occurrences.append(idx)
        pos = idx + len(fn_name)
    if not occurrences:
        return ""
    # For hooks.cpp the extern "C" wrapper is the second occurrence.
    # For audit.h the function definition is the first occurrence after
    # the declaration noise. Heuristic: pick the last occurrence.
    start = occurrences[-1]
    return src[start : start + WINDOW]


def check_site(path: Path, fn_name: str, or_markers: list[str]) -> list[str]:
    """Return list of failure messages (empty if pass)."""
    if not path.exists():
        return [f"{path}: file not found"]
    try:
        src = path.read_text(encoding="utf-8")
    except OSError as e:
        return [f"{path}: {e}"]
    body = find_function_body(src, fn_name)
    if not body:
        return [f"{path}:{fn_name}: function body not found"]
    failures = []
    # OR semantics: every marker must be present.
    for marker in or_markers:
        if marker not in body:
            failures.append(f"{path}:{fn_name}: missing OR-semantics marker {marker!r}")
    # The old max-style pick must be absent.
    if MAX_PICK_SNIPPET in body:
        failures.append(f"{path}:{fn_name}: max-style pick still present (OR semantics required)")
    return failures


def run_self_test() -> list[str]:
    """Inline AC matrix — verify the OR logic blocks override=0 + actual
    depth>0 (the #3558 race window). Python-side truth table; the C++
    side is enforced by the patch + source-cite test."""
    failures: list[str] = []

    def or_block(override: int, depth: int, pending: int = 0) -> int:
        if override > 0:
            return 1
        if depth > 0:
            return 1
        if pending > 0:
            return 1
        return 0

    cases = [
        # (override, depth, pending, expected_block, label)
        (-1, 0, 0, 0, "quiet — both signals off"),
        (-1, 1, 0, 1, "depth only"),
        (1, 0, 0, 1, "override only"),
        (1, 1, 0, 1, "both signals"),
        (0, 1, 0, 1, "OVERRIDE=0 RACE (the bug scenario — must block)"),
        (0, 0, 1, 1, "pending only"),
        (-1, 0, 1, 1, "pending only (override unset)"),
        (0, 0, 0, 0, "all quiet — must NOT block"),
        (0, 0, 1, 1, "override=0 + pending only"),
    ]
    for override, depth, pending, expected, label in cases:
        got = or_block(override, depth, pending)
        if got != expected:
            failures.append(
                f"self-test: case {label!r} "
                f"(override={override}, depth={depth}, pending={pending}) "
                f"expected block={expected}, got {got}"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on any violation (default: warnings only)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run inline AC matrix (override=0 race-window must block)",
    )
    args = parser.parse_args()

    failures: list[str] = []
    for path, fn_name, or_markers in SITES:
        failures.extend(check_site(path, fn_name, or_markers))

    if args.self_test:
        failures.extend(run_self_test())

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if args.strict or args.self_test:
            return 1
        return 0
    if args.self_test:
        print("PASS: all 3 sites use OR semantics; self-test race-window case (override=0, depth>0) blocks as required")
    else:
        print("PASS: linear fast-path OR-semantics clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
