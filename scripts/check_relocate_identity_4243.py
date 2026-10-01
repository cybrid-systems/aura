#!/usr/bin/env python3
# scripts/check_relocate_identity_4243.py -- Issue #4243 source-cite gate.
#
# Verifies the minimal relocate-identity fix: the #3464 collision arm in
# ASTArena::relocate_tracked_objects_for_moving_ must NEVER drop the loser's
# DtorEntry (identity) when its old slot was recycled and reused; it restores
# the loser to a fresh tracked address instead. Plus the caller-side tighten:
# live_compact's invalidate skip set must be the set of destinations whose
# pins remap_pins_pointing_to actually rewrote this window
# (pins_rewritten_new), NOT the raw set of this-window remap values.
#
#  AC1: collision arm never drops — kept.push_back(DtorEntry{fresh ? fresh :
#       p.old, ...}) present; the old "drop this identity from dtors_" arm is
#       gone; the defensive g_relocate_collision_restore_total counter bumps;
#       the #3435 restore-to-old anchor is preserved for the non-collision arm.
#  AC2: invalidate skip set = pins_rewritten_new, filled only when
#       remap_pins_pointing_to actually rewrote a pin this window; the landed
#       #2374 helper (membership-only skipping) is deliberately unchanged.
#  AC3: the lane's recycle-after-success reordering / deferred
#       last_object_remap_ publish is reverted — the up-front recycle and the
#       per-iteration last_object_remap_[p.old] = neu publish stay (the #4242
#       quarantine pred keeps working).
#  AC4: test extension lives in tests/core/test_moving_densify_fail_closed.cpp
#       (ac4243_1..4; no tests/**/test_issue_4243.cpp per #81934; no
#       docs/design/4243-* per #1655); build.py + allowlist wiring present.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/core/arena.ixx",
    "src/core/lifetime_pin.hh",
    "tests/core/test_moving_densify_fail_closed.cpp",
    "build.py",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: collision arm never drops the identity.
    (
        "src/core/arena.ixx",
        r"Issue\s+#4243",
        "4243 AC1: arena.ixx cites #4243",
    ),
    (
        "src/core/arena.ixx",
        r"kept\.push_back\(DtorEntry\{fresh \? fresh : p\.old, p\.dtor, p\.size, p\.align\}\)",
        "4243 AC1: collision loser restored to a tracked address (never dropped)",
    ),
    (
        "src/core/arena.ixx",
        r"g_relocate_collision_restore_total\.fetch_add",
        "4243 AC1: defensive collision-restore counter bumped",
    ),
    (
        "src/core/arena.ixx",
        r"kept\.push_back\(DtorEntry\{p\.old, p\.dtor, p\.size, p\.align\}\)",
        "4243 AC1: #3435 restore-to-old anchor preserved",
    ),
    # AC2: caller-side invalidate skip-set tighten.
    (
        "src/core/arena.ixx",
        r"pins_rewritten_new",
        "4243 AC2: actually-rewritten destination set present",
    ),
    (
        "src/core/arena.ixx",
        r"pins_rewritten_new\.insert\(new_ptr\)",
        "4243 AC2: set filled from remap_pins_pointing_to rewrites",
    ),
    (
        "src/core/arena.ixx",
        r"pins_rewritten_new\);",
        "4243 AC2: invalidate consumes the rewritten set",
    ),
    (
        "src/core/lifetime_pin.hh",
        r"Issue\s+#4243",
        "4243 AC2: lifetime_pin.hh documents the tightened caller contract",
    ),
    # AC3: reordering reverted (up-front recycle + per-iteration publish).
    (
        "src/core/arena.ixx",
        r"last_object_remap_\[p\.old\] = neu",
        "4243 AC3: per-iteration remap publish restored (reordering reverted)",
    ),
    # AC4: wiring + runtime ACs.
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"ac4243_1_alloc_fail_never_drops_identity",
        "4243 AC4: AC1 present",
    ),
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"ac4243_2_no_pin_window_green",
        "4243 AC4: AC2 present",
    ),
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"ac4243_3_invalidate_skip_set_is_rewritten",
        "4243 AC4: AC3 present",
    ),
    (
        "tests/core/test_moving_densify_fail_closed.cpp",
        r"ac4243_4_source_and_linter",
        "4243 AC4: AC4 present",
    ),
    (
        "build.py",
        r"check_relocate_identity_4243",
        "4243 AC4: build.py wires the linter",
    ),
    (
        "scripts/coverage/root_check_allowlist.txt",
        r"^check_relocate_identity_4243\.py$",
        "4243 AC4: linter on the frozen root allowlist",
    ),
)

# (path, regex, label) -- each regex must NOT appear.
FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        "src/core/arena.ixx",
        r"drop this identity from dtors_",
        "4243 AC1: the identity-drop arm must stay removed (never drop)",
    ),
    (
        "src/core/arena.ixx",
        r"window_remap_pairs",
        "4243 AC3: the deferred-publish scratch must stay reverted",
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
        if re.search(pattern, body(path), flags=re.MULTILINE) is None:
            failures.append(label)

    for path, pattern, label in FORBIDDEN:
        if re.search(pattern, body(path)) is not None:
            failures.append(label)

    # AC2: bounded section — the invalidate call must consume the rewritten
    # set, not a locally rebuilt remap-value set. Anchor is format-stable:
    # search after the callee open-paren (clang-format may reflow the arg
    # list across lines).
    arena = body("src/core/arena.ixx")
    i0 = arena.find("invalidate_pins_not_in_new_addrs(")
    if i0 == -1:
        failures.append("4243 AC2: invalidate call site not found (anchor drift)")
    else:
        call_tail = arena[i0 : i0 + 200]
        if "pins_rewritten_new" not in call_tail or "arena_id_" not in call_tail:
            failures.append("4243 AC2: invalidate must consume pins_rewritten_new")

    # AC4: no design doc, no standalone issue test.
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("4243-"):
                failures.append(f"4243 AC4: docs/design/{entry.name} must not exist (#1655)")
    if (REPO_ROOT / "tests" / "core" / "test_issue_4243.cpp").exists():
        failures.append("4243 AC4: tests/core/test_issue_4243.cpp must not exist (#81934)")
    if (REPO_ROOT / "tests" / "issues" / "test_issue_4243.cpp").exists():
        failures.append("4243 AC4: tests/issues/test_issue_4243.cpp must not exist (#81934)")

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
    for _, pattern, label in FORBIDDEN:
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
    ap = argparse.ArgumentParser(description="Issue #4243 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any failure")
    ap.add_argument("--self-test", action="store_true", help="regex + anchor pre-flight")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    failures = run_checks()
    for f in failures:
        print(f"FAIL: {f}")
    if failures:
        print(f"check_relocate_identity_4243: {len(failures)} row(s) failed")
        return 1 if args.strict else 0
    print("check_relocate_identity_4243: all rows clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
