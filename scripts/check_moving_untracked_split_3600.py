#!/usr/bin/env python3
# scripts/check_moving_untracked_split_3600.py -- Issue #3600 source-cite gate.
#
# Verifies the kept-large / untracked-external split in Moving relocate
# (ASTArena::relocate_tracked_objects_for_moving_):
#
#  AC1: kept-large + degenerate dtor entries no longer increment the
#       incomplete counter — they are tracked, address-stable entries on
#       dtors_, NOT external-root misses. The bare `++untracked_kept`
#       increments and the local `std::size_t untracked_kept = 0;` are gone.
#  AC2: the out-parameter still carries true external identity-drops
#       (alloc-fail restore #3435 / same-window collision #3464:
#       ++*out_untracked_kept_count).
#  AC3: pre-densify external-root SSOT preserved:
#       count_pre_densify_untracked_external_roots_() still walks declared
#       roots and still writes result.untracked_kept_count; the #2973/#3017
#       hard gate stays.
#  AC4: fail-closed gate shape unchanged: objects_moved > 0 &&
#       (untracked_kept_count > 0 || stale_unremapped > 0) ->
#       moving_incomplete_remap + pin_contract_held=false +
#       g_moving_untracked_external_roots_total (no new metric key).
#  AC5: test extension lives in tests/core/test_moving_densify_fail_closed.cpp
#       (ac3600_1..4 + PodLarge3600); no tests/**/test_issue_3600.cpp
#       (#81934); no docs/design/3600-* (#1655).

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = ("src/core/arena.ixx", "tests/core/test_moving_densify_fail_closed.cpp")

# (path, regex, label) -- each tuple is a regex pattern that must appear.
REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: split anchors.
    (
        "src/core/arena.ixx",
        r"Issue\s+#3600",
        "3600 AC1: arena.ixx cites #3600",
    ),
    (
        "src/core/arena.ixx",
        r"kept-large is a tracked, address-stable dtor",
        "3600 AC1: kept-large branch documents the not-an-external-miss semantics",
    ),
    (
        "src/core/arena.ixx",
        r"no local untracked counter",
        "3600 AC1: local untracked_kept counter removed (#3600)",
    ),
    # AC2: external identity-drop bucket preserved.
    (
        "src/core/arena.ixx",
        r"\+\+\*out_untracked_kept_count",
        "3600 AC2: alloc-fail / collision still increments the external bucket",
    ),
    # AC3: pre-densify external-root SSOT preserved.
    (
        "src/core/arena.ixx",
        r"count_pre_densify_untracked_external_roots_\(\)",
        "3600 AC3: pre-densify external-root walk preserved",
    ),
    (
        "src/core/arena.ixx",
        r"result\.untracked_kept_count\s*=\s*untracked;",
        "3600 AC3: pre-densify path still publishes the external count",
    ),
    # AC4: fail-closed gate shape unchanged; existing counter reused.
    (
        "src/core/arena.ixx",
        r"\(result\.untracked_kept_count > 0 \|\| stale_unremapped > 0\)",
        "3600 AC4: gate still consumes untracked_kept_count + stale_unremapped",
    ),
    (
        "src/core/arena.ixx",
        r"g_moving_untracked_external_roots_total\.fetch_add",
        "3600 AC4: existing external-roots counter reused (no new metric key)",
    ),
    # AC5: test extension + wiring.
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"ac3600_1_mixed_size_green_window",
        "3600 AC5: AC1 mixed-size green window present",
    ),
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"ac3600_2_external_only_still_red",
        "3600 AC5: AC2 external-only red present",
    ),
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"ac3600_3_soft_mixed_no_sticky",
        "3600 AC5: AC3 soft no-sticky present",
    ),
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"ac3600_4_source_and_linter",
        "3600 AC5: AC4 source-cite present",
    ),
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"PodLarge3600",
        "3600 AC5: >kMaxSmallSize tracked fixture present",
    ),
    (
        "build.py",
        r"check_moving_untracked_split_3600",
        "3600 AC5: build.py wires the linter",
    ),
)

# (path, regex, label) -- each regex must NOT appear.
FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        "src/core/arena.ixx",
        r"\+\+untracked_kept\b",
        "3600 AC1: bare kept-large increment must stay removed",
    ),
    (
        "src/core/arena.ixx",
        r"std::size_t untracked_kept\s*=\s*0;",
        "3600 AC1: local untracked_kept counter must stay removed",
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

    # AC1: bounded section — the kept-large branch keeps its entry and does
    # not touch the counter. Anchor drift -> explicit failure.
    arena = body("src/core/arena.ixx")
    a0 = arena.find("Only densify freelist-reclaimable small-pool objects.")
    a1 = arena.find("Pending p;", a0 + 1) if a0 != -1 else -1
    if a0 == -1 or a1 == -1 or a1 <= a0:
        failures.append("3600 AC1: kept-large section not bounded (anchor drift)")
    else:
        section = arena[a0:a1]
        if "kept.push_back(e);" not in section:
            failures.append("3600 AC1: kept-large branch must retain the entry on dtors_")
        if "untracked_kept" in section:
            failures.append("3600 AC1: kept-large branch must not touch the incomplete counter")

    # AC5: no design doc, no standalone issue test.
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3600-"):
                failures.append(f"3600 AC5: docs/design/{entry.name} must not exist (#1655)")
    if (REPO_ROOT / "tests" / "core" / "test_issue_3600.cpp").exists():
        failures.append("3600 AC5: tests/core/test_issue_3600.cpp must not exist (#81934)")
    if (REPO_ROOT / "tests" / "issues" / "test_issue_3600.cpp").exists():
        failures.append("3600 AC5: tests/issues/test_issue_3600.cpp must not exist (#81934)")

    return failures


def self_test() -> int:
    """Regexes compile + anchor texts exist in the targets (pre-flight)."""
    ok = True
    for _, pattern, label in REQUIRED:
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
    ap = argparse.ArgumentParser(description="Issue #3600 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    ap.add_argument("--self-test", action="store_true", help="regex + anchor pre-flight")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    failures = run_checks()
    for f in failures:
        print(f"FAIL: {f}")
    if failures:
        print(f"check_moving_untracked_split_3600: {len(failures)} row(s) failed")
        return 1 if args.strict else 0
    print("check_moving_untracked_split_3600: all rows clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
