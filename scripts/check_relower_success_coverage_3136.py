#!/usr/bin/env python3
# scripts/check_relower_success_coverage_3136.py -- Issue #3136 source-cite gate.
#
# Verifies the relower-success-path bitmap coherence infrastructure is in
# place across the two layers:
#
#  1. Producer hook (src/compiler/hot_update_registry.hh):
#     - note_relower_success_coverage(std::uint64_t) noexcept exists
#     - last_reemit_success_region_mask_ atomic field exists
#     - last_reemit_success_region_mask() noexcept accessor exists
#
#  2. Call sites (4 in src/compiler/service.ixx + 2 in
#     src/compiler/service_dirty.cpp), each with the inline
#     `aura_production_defaults_active_probe() != 0` gate before the
#     `note_relower_success_coverage(...)` call. Service.ixx uses
#     `fnv1a_64(name) & 63` directly; service_dirty.cpp uses the shared
#     `relower_success_region_bit(name_or_d)` helper (inlined fnv1a_64
#     in hot_update_registry.hh — #3383 unified the bitmap identity so
#     both call sites stamp the same bit for the same define).
#
#  3. Soft / Off zero-cost guarantee: each call site gates BEFORE
#     `fnv1a_64(...)` / `std::hash<...>(...)` so the hash computation
#     is skipped under Soft / Off (no extra work on the cold path).

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/hot_update_registry.hh",
    "src/compiler/service.ixx",
    "src/compiler/service_dirty.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
# The "gate-before-hash" pattern is the critical Soft / Off zero-cost
# invariant: `aura_production_defaults_active_probe() != 0` MUST appear
# before the hash computation (fnv1a_64 OR std::hash<std::string_view>)
# within a 6-line window after restamp_cache_entry_live_.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Producer hook (src/compiler/hot_update_registry.hh)
    ("src/compiler/hot_update_registry.hh", r"note_relower_success_coverage\(std::uint64_t", "producer hook decl"),
    ("src/compiler/hot_update_registry.hh", r"last_reemit_success_region_mask_\{0\}", "success mask atomic field"),
    (
        "src/compiler/hot_update_registry.hh",
        r"last_reemit_success_region_mask\(\)\s+const\s+noexcept",
        "success mask accessor",
    ),
    # Service.ixx — 4 call sites (store_ir_cache_v2 / partial peel /
    # per-fn partial / test helper restamp_cache_entry_for_test). Each
    # requires the inline gate + fnv1a_64-based bit.
    (
        "src/compiler/service.ixx",
        r"note_relower_success_coverage\(1ULL << \(fnv1a_64\(name\) & 63\)\)",
        "service.ixx fnv1a_64 call sites",
    ),
    ("src/compiler/service.ixx", r"aura_production_defaults_active_probe\(\)\s*!=\s*0", "service.ixx inline gate"),
    # Service_dirty.cpp — 2 call sites (root restamp with `name` +
    # dependent restamp with `d`). Uses the shared
    # `relower_success_region_bit(name_or_d)` helper (inlined fnv1a_64,
    # same hash as service.ixx `store_define_v2` — #3383 unified the
    # bitmap identity so both call sites stamp the same bit for the
    # same define). Single regex covers both via `\w+`.
    (
        "src/compiler/service_dirty.cpp",
        r"note_relower_success_coverage\(\s*relower_success_region_bit\(\w+\)\)",
        "service_dirty.cpp root + dependent call sites (shared fnv1a_64 helper)",
    ),
    (
        "src/compiler/service_dirty.cpp",
        r"aura_production_defaults_active_probe\(\)\s*!=\s*0",
        "service_dirty.cpp inline gate",
    ),
    # Test + manifest presence
    (
        "tests/compiler/test_hot_update_relower_success_coverage.cpp",
        r"Issue #3136",
        "test_hot_update_relower_success_coverage.cpp present",
    ),
)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def check_file(rel_path: str, regex: str, *, strict: bool) -> list[str]:
    failures: list[str] = []
    p = REPO_ROOT / rel_path
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    if not re.search(regex, text) and strict:
        failures.append(f"{rel_path}: missing required pattern: {regex!r}")
    return failures


def check_gate_before_hash(rel_path: str, *, strict: bool) -> list[str]:
    """Verify Soft / Off zero-cost invariant: gate before hash within a 6-line window.

    For each `note_relower_success_coverage(...)` call in the file, the
    `aura_production_defaults_active_probe() != 0` gate must appear on a
    line that precedes the `note_relower_success_coverage` line (within
    6 lines above). This ensures the hash computation (fnv1a_64 OR
    std::hash) is gated by the production probe, not unconditionally
    computed.
    """
    failures: list[str] = []
    p = REPO_ROOT / rel_path
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    lines = text.splitlines()
    GATE_RX = re.compile(r"aura_production_defaults_active_probe\(\)\s*!=\s*0")
    CALL_RX = re.compile(r"hot_update_registry\(\)\.note_relower_success_coverage\(")
    WINDOW = 6  # max lines from gate to call (inclusive)
    call_lines = [i for i, ln in enumerate(lines) if CALL_RX.search(ln)]
    if not call_lines and strict:
        failures.append(f"{rel_path}: no `note_relower_success_coverage` call sites found")
        return failures
    for cl in call_lines:
        # Look in the WINDOW lines ABOVE cl for a gate. Ignore comment lines.
        gated = False
        for j in range(max(0, cl - WINDOW), cl):
            if GATE_RX.search(lines[j]):
                gated = True
                break
        if not gated and strict:
            failures.append(
                f"{rel_path}: line {cl + 1}: note_relower_success_coverage call "
                f"is not gated by aura_production_defaults_active_probe() within "
                f"{WINDOW} lines above (Soft / Off zero-cost invariant violated)"
            )
    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture = """
    // hot_update_registry.hh
    void note_relower_success_coverage(std::uint64_t region_bit) noexcept;
    std::atomic<std::uint64_t> last_reemit_success_region_mask_{0};
    std::uint64_t last_reemit_success_region_mask() const noexcept;
    // service.ixx — 4 sites, each gated
    restamp_cache_entry_live_(entry);
    if (aura_production_defaults_active_probe() != 0)
        hot_update_registry().note_relower_success_coverage(1ULL << (fnv1a_64(name) & 63));
    // service_dirty.cpp — 2 sites, each gated, shared fnv1a_64 helper (#3383)
    if (aura_production_defaults_active_probe() != 0)
        hot_update_registry().note_relower_success_coverage(
            relower_success_region_bit(name));
    // test
    // Issue #3136 — relower-success-path bitmap coherence
    """
    fails: list[str] = []
    for rel, rx, label in INFRA_REQUIRED:
        if (
            "hot_update_registry.hh" in rel
            or "service.ixx" in rel
            and "fnv1a" in rx
            or "service.ixx" in rel
            and "gate" in label
            or "service_dirty.cpp" in rel
            and "gate" in label
            or "service_dirty.cpp" in rel
            or "test_hot_update_relower_success_coverage" in rel
        ):
            text = fixture
        else:
            text = ""
        if not re.search(rx, text):
            fails.append(f"{rel}: missing required pattern: {rx!r} (label: {label})")
    # gate-before-hash check
    for rel in ("src/compiler/service.ixx", "src/compiler/service_dirty.cpp"):
        fails.extend(check_gate_before_hash(rel, strict=True) if rel in fixture else [])
    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns found in fixture")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3136 relower-success-path bitmap coherence source-cite gate",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=True,
        help="Fail on missing patterns (default: strict)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run the linter self-test and exit",
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="Files to scan (default: hot_update_registry.hh + service.ixx + service_dirty.cpp)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures: list[str] = []
    target_filter = set(args.targets) if args.targets else set(DEFAULT_TARGETS)
    for rel_path, regex, _label in INFRA_REQUIRED:
        if rel_path not in target_filter:
            continue
        failures.extend(check_file(rel_path, regex, strict=args.strict))
    # gate-before-hash invariant — only check service.ixx + service_dirty.cpp
    for rel_path in ("src/compiler/service.ixx", "src/compiler/service_dirty.cpp"):
        if rel_path not in target_filter:
            continue
        failures.extend(check_gate_before_hash(rel_path, strict=args.strict))

    if failures:
        print("check_relower_success_coverage_3136: FAIL")
        for line in failures:
            print(f"  {line}")
        return 1

    print("check_relower_success_coverage_3136: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
