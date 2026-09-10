#!/usr/bin/env python3
# scripts/check_region_storm_attribution_3636.py -- Issue #3636 source-cite gate.
#
# Verifies deopt-storm reemit throttling carries per-region attribution:
# force bits stamp a first-armed watermark, soft-storm passes attribute the
# skipped dirty-region mask, region-scoped soft throttle defers only the
# attributed candidates (hard ceiling still throttles everyone, critical
# bypass preserved), and the health face exposes the advisory
# region-force-starve signal without changing the score.
#
#  AC1: per-bit first-armed watermark (end-append array + stamp at the
#       force-arm fetch_or + aggregate merge; no re-stamp while set),
#       published via the registry snapshot (schema_3636 tail).
#  AC2: soft storm attributes the cause mask + falls through with a region
#       scope; candidates in attributed bits defer, non-intersecting
#       proceed; critical bypass per-candidate; hard ceiling early-return
#       (throttle-all) unchanged.
#  AC3: health advisory region-force-starve — threshold constant, advisory
#       reason, no health_bp / force_reason change (pure face).
#  AC4: quiet path — the scoped skip is guarded by storm_scope_mask != 0
#       (zero cost when no storm); Soft/Off unchanged.
#  AC5: test face in test_region_priority_deopt_throttle.cpp (watermark,
#       dual-region scoped storm, hard ceiling, advisory purity).
#  AC6: no docs/design/3636-* (#1655); no tests/**/test_issue_3636.cpp
#       (#81934); build.py wires this linter.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

HH = "src/compiler/hot_update_registry.hh"
CPP = "src/compiler/hot_update_registry.cpp"
BRIDGE = "src/compiler/aura_jit_bridge.cpp"
HEALTH = "src/compiler/aot_hot_update_health.hh"
TEST = "tests/compiler/test_hot_update_cascade_dirty_reemit.cpp"

REQUIRED: tuple[tuple[str, str, str], ...] = (
    (HH, r"region_force_first_armed_ms_\[64\]", "3636 AC1: per-bit watermark array (end-append)"),
    (HH, r"note_throttle_cause_mask", "3636 AC2: cause-mask setter declared"),
    (HH, r"schema_3636 = 3636", "3636 AC1: snapshot tail schema_3636"),
    (CPP, r"Issue\s+#3636", "3636 AC1: watermark impl cites #3636"),
    (CPP, r"note_force_bits_armed\(prev_mask, new_mask\)", "3636 AC1: force-arm site stamps watermark"),
    (CPP, r"max_region_force_age_ms", "3636 AC1: max-age accessor"),
    (BRIDGE, r"note_throttle_cause_mask\(dirty_mask\)", "3636 AC2: soft throttle attributes cause mask"),
    (BRIDGE, r"storm_scope_mask = region_or_prio", "3636 AC2: scoped soft storm falls through"),
    (BRIDGE, r"bump_reemit_soft_storm_region_skips\(storm_scope_skips\)", "3636 AC2: scoped skips counted"),
    (BRIDGE, r"!hur\.is_critical_region\(cbit\)", "3636 AC2: critical bypass per-candidate preserved"),
    (HEALTH, r"region-force-starve", "3636 AC3: advisory escalation reason"),
    (HEALTH, r"kRegionForceStarveAdvisoryMs = 30000", "3636 AC3: advisory threshold constant"),
    (TEST, r"3636 AC7", "3636 AC5: test watermark face"),
    (TEST, r"reemit_soft_storm_region_skips", "3636 AC5: test asserts scoped skips"),
    (TEST, r"hard ceiling throttles all", "3636 AC5: test hard-ceiling throttle-all"),
    ("build.py", r"check_region_storm_attribution_3636", "3636 AC6: build.py wires the linter"),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


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

    bridge = body(BRIDGE)

    # AC2: the hard ceiling early-return must sit BEFORE the scope fall-through
    # (hard throttles everyone; only soft storms scope), and the candidate-loop
    # skip must come after the walk starts (dirty_by_region accumulation).
    hard_pos = bridge.find("if (hard || region_or_prio == 0) {")
    scope_pos = bridge.find("storm_scope_mask = region_or_prio;")
    if hard_pos == -1 or scope_pos == -1:
        failures.append("3636 AC2: hard early-return / scope fall-through anchors not found")
    elif not (hard_pos < scope_pos):
        failures.append("3636 AC2: hard ceiling early-return must precede the scope fall-through")

    # AC4: the scoped skip is guarded by storm_scope_mask != 0 (quiet path
    # unchanged when no storm) and sits after the dirty_by_region accumulation.
    acc_pos = bridge.find("dirty_by_region[region] += 1;")
    skip_pos = bridge.find("if (storm_scope_mask != 0 && region != 0) {")
    if acc_pos == -1 or skip_pos == -1:
        failures.append("3636 AC4: quiet-path guard anchors not found")
    elif not (acc_pos < skip_pos):
        failures.append("3636 AC4: scoped skip must sit after the dirty_by_region accumulation")

    # AC3: the advisory reason must not change the score — compute tail only
    # reads region_force_starve; no penalty row references it.
    health = body(HEALTH)
    compute = health[health.find("compute_aot_hot_update_health") :]
    adv = compute.find("advisory_reason")
    bp_end = compute.find("r.health_bp = ")
    if adv == -1 or bp_end == -1 or adv < bp_end:
        failures.append("3636 AC3: advisory reason must be computed after the score (no bp change)")

    # AC6: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3636-"):
                failures.append(f"3636 AC6: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3636.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3636.cpp",
    ):
        if probe.exists():
            failures.append(f"3636 AC6: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def self_test() -> int:
    """Regexes compile + anchor targets exist (pre-flight)."""
    ok = True
    for _, pattern, label in REQUIRED:
        try:
            re.compile(pattern)
        except re.error as e:
            print(f"SELF-TEST FAIL (regex compile): {label}: {e}")
            ok = False
    for path, _, _ in REQUIRED:
        if not (REPO_ROOT / path).exists():
            print(f"SELF-TEST FAIL (missing target): {path}")
            ok = False
    if ok:
        print("self-test: regexes compile + targets present")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3636 region storm attribution + force watermark gate")
    ap.add_argument("--self-test", action="store_true", help="compile regexes + probe targets")
    ap.add_argument("--strict", action="store_true", help="exit non-zero on any failure")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    failures = run_checks()
    if failures:
        print(f"check_region_storm_attribution_3636: {len(failures)} failure(s)")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1 if args.strict else 0
    print("check_region_storm_attribution_3636: clean (per-region watermark + scoped soft storm + advisory health)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
