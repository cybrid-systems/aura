#!/usr/bin/env python3
# scripts/check_ffi_apply_densify_refuse_3602.py -- Issue #3602 source-cite gate.
#
# Verifies the FFI arm of Evaluator::apply_closure shares the #3421
# densify-stale hard-refuse (#3533/#3443 residual): the TW/IR arms refused;
# the FFI return path marshaled + called native with no consult.
#
#  AC1: production_ffi_apply_densify_hard_refuse present in
#       evaluator_eval_flat.cpp (definition + FFI-arm call site), citing
#       Issue #3602; the call sits after ffi_marshal_args_pure and before
#       the native call (any_float dispatch), reusing the shared note
#       helper (closure_stale_returns + compiler_root_dangling).
#  AC2: ffi_marshal_args_pure stays pure - evaluator_pure.ixx must NOT
#       consult resolve_object_remap (remap happens at the apply gate).
#  AC3: test face in test_setcode_rebind_survive.cpp (c-func registration,
#       c-opaque stale-arg fabrication, ProdDensifyWindowGuard production
#       window, remap-key refuse + post-rewrite allow).
#  AC4: no docs/design/3602-* (#1655); no tests/**/test_issue_3602.cpp
#       (#81934); build.py wires this linter.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

FLAT = "src/compiler/evaluator_eval_flat.cpp"
PURE = "src/compiler/evaluator_pure.ixx"
TEST = "tests/compiler/test_setcode_rebind_survive.cpp"

DEFAULT_TARGETS: tuple[str, ...] = (FLAT, PURE, TEST, "build.py")

REQUIRED: tuple[tuple[str, str, str], ...] = (
    (
        FLAT,
        r"Issue\s+#3602",
        "3602 AC1: lockless/ffi helper cites #3602",
    ),
    (
        FLAT,
        r"resolve_object_remap",
        "3602 AC1: FFI refuse consults resolve_object_remap",
    ),
    (
        TEST,
        r"Issue\s+#3602",
        "3602 AC3: test hosts #3602 ACs",
    ),
    (
        TEST,
        r"c-func",
        "3602 AC3: test registers the FFI fn via c-func (RTLD_DEFAULT)",
    ),
    (
        TEST,
        r"c-opaque",
        "3602 AC3: test fabricates the stale opaque arg via c-opaque",
    ),
    (
        TEST,
        r"ProdDensifyWindowGuard",
        "3602 AC3: test drives the production densify window",
    ),
    (
        "build.py",
        r"check_ffi_apply_densify_refuse_3602",
        "3602 AC4: build.py wires the linter",
    ),
)

FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        PURE,
        r"resolve_object_remap",
        "3602 AC2: ffi_marshal_args_pure stays pure (no remap consult in marshal)",
    ),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def run_checks() -> list[str]:
    """Returns a list of failure labels (empty = clean)."""
    failures: list[str] = []
    cache: dict[str, str] = {}

    def body(path: str) -> str:
        if path not in cache:
            full = REPO_ROOT / path
            cache[path] = read(full) if full.exists() else ""
        return cache[path]

    for path, pattern, label in REQUIRED:
        if re.search(pattern, body(path)) is None:
            failures.append(label)

    for path, pattern, label in FORBIDDEN:
        if re.search(pattern, body(path)) is not None:
            failures.append(label)

    flat = body(FLAT)

    # AC1: helper defined AND wired at the FFI entry (>= 2 occurrences:
    # definition + call site).
    count = flat.count("production_ffi_apply_densify_hard_refuse")
    if count < 2:
        failures.append(f"3602 AC1: expected >=2 helper refs (def + call), found {count}")

    # AC1: the FFI-arm call sits after the marshal and before the native
    # dispatch (any_float branch) - same #3217 ordering contract: refuse
    # precedes any native call / workspace effect.
    marshal_pos = flat.find("ffi_marshal_args_pure(")
    dispatch_pos = flat.find("if (marshalled.any_float)", marshal_pos + 1)
    call_pos = flat.find("production_ffi_apply_densify_hard_refuse(arena_", marshal_pos + 1)
    if marshal_pos == -1 or dispatch_pos == -1 or call_pos == -1:
        failures.append("3602 AC1: FFI arm anchors not found (marshal/dispatch/call)")
    elif not (marshal_pos < call_pos < dispatch_pos):
        failures.append("3602 AC1: FFI refuse call must sit after ffi_marshal_args_pure and before the native dispatch")

    # AC1: shared note helper reused at the call site (closure_stale_returns
    # + compiler_root_dangling via note_apply_closure_densify_hard_refuse).
    if call_pos != -1:
        window = flat[call_pos : call_pos + 600]
        if "note_apply_closure_densify_hard_refuse" not in window:
            failures.append("3602 AC1: refuse call must reuse the shared note helper")

    # AC4: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3602-"):
                failures.append(f"3602 AC4: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3602.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3602.cpp",
    ):
        if probe.exists():
            failures.append(f"3602 AC4: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def self_test() -> int:
    """Regexes compile + anchor targets exist (pre-flight)."""
    ok = True
    for _, pattern, label in REQUIRED + FORBIDDEN:
        try:
            re.compile(pattern)
        except re.error as e:
            print(f"SELF-TEST FAIL (regex compile): {label}: {e}")
            ok = False
    for path in DEFAULT_TARGETS:
        if not (REPO_ROOT / path).exists():
            print(f"SELF-TEST FAIL (missing target): {path}")
            ok = False
    if ok:
        print("self-test: regexes compile + targets present")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3602 FFI apply densify refuse gate")
    ap.add_argument("--self-test", action="store_true", help="compile regexes + probe targets")
    ap.add_argument("--strict", action="store_true", help="exit non-zero on any failure")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    failures = run_checks()
    if failures:
        print(f"check_ffi_apply_densify_refuse_3602: {len(failures)} failure(s)")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1 if args.strict else 0
    print("check_ffi_apply_densify_refuse_3602: clean (FFI arm shares the #3421 refuse)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
