#!/usr/bin/env python3
# scripts/check_facade_owner_scope_clock_skip_3605.py -- Issue #3605 gate.
#
# Verifies the production owner-scoped facade freezes the process
# C-bridge / defuse clocks (#3300 contract lockstep; #2841 isolation):
#
#  AC1: hard_invalidate_via_facade consults aura_aot_bump_will_be_owner_
#       scoped() BEFORE the joint C bump; on the owner-scoped path the two
#       process bumpers are skipped (single-eval / force / env-opt-out
#       keep the #3150 joint bump) and the hard-owner-scoped note is
#       armed BEFORE the table bump (drives bumper attribution + the
#       all-slot peer-mark gate).
#  AC2: aura_jit_bridge.cpp defines the non-consuming probe (same inputs
#       as the bumper's owner branch; never writes the force TLS) and the
#       last-bump-scope flag read by the stamp.
#  AC3: the bumper's owner branch gates aura_aot_mark_peer_slots_soft_
#       stale on !hard_os_pref — the hard owner-scoped facade path no
#       longer soft-stales ALL non-owner slots (over-cover); soft cascade
#       keeps #3070.
#  AC4: stamp_eval_core_joint_after_production_facade_ skips the core
#       bridge epoch bump + C mirror SET + Evaluator defuse dual-write on
#       the owner-scoped path (last-bump-scope flag); all #3219 dual-write
#       rows stay present for the global path.
#  AC5: the #3300 peer-JIT-name comment cites #3605 (contract reworded:
#       bridge_epoch is not advanced on the owner-scoped path).
#  AC6: test face in test_compiler_hot_update_facade.cpp; build.py wires
#       this linter; no docs/design/3605-* (#1655); no tests/**/test_
#       issue_3605.cpp (#81934).

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

HUR = "src/compiler/hot_update_registry.cpp"
BRIDGE = "src/compiler/aura_jit_bridge.cpp"
BRIDGE_H = "src/compiler/aura_jit_bridge.h"
SVC = "src/compiler/service.ixx"
TEST = "tests/compiler/test_compiler_hot_update_facade.cpp"
BUILD = "build.py"

REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: facade skip.
    (HUR, r"Issue\s+#3605", "3605 AC1: facade cites #3605"),
    (HUR, r"aura_aot_bump_will_be_owner_scoped\(\)", "3605 AC1: facade consults the scope probe"),
    (HUR, r"if \(c_clocks_owner_skipped\)", "3605 AC1: skip guard present"),
    (
        HUR,
        r"aura_hot_update_bump_bridge_epoch\(\);",
        "3605 AC1: global path keeps the #3150 C-bridge bump",
    ),
    (
        HUR,
        r"aura_hot_update_bump_defuse_version\(\);",
        "3605 AC1: global path keeps the #3150 defuse bump",
    ),
    # AC2: probe + scope flag in the bridge TU.
    (BRIDGE, r"Issue\s+#3605", "3605 AC2: bridge cites #3605"),
    (BRIDGE_H, r"aura_aot_bump_will_be_owner_scoped", "3605 AC2: probe declared in bridge.h"),
    (
        BRIDGE_H,
        r"aura_aot_last_table_bump_owner_scoped",
        "3605 AC2: scope reader declared in bridge.h",
    ),
    (BRIDGE, r"g_last_table_bump_owner_scoped", "3605 AC2: last-bump-scope flag defined"),
    # AC3: all-slot peer mark gated on the hard-owner-scoped path.
    (BRIDGE, r"if \(!hard_os_pref\)", "3605 AC3: peer-slot mark gated on !hard_os_pref"),
    # AC4: stamp gate.
    (SVC, r"aura_aot_last_table_bump_owner_scoped", "3605 AC4: stamp reads the scope flag"),
    (SVC, r"if \(!os_table_bump\)", "3605 AC4: stamp gate present"),
    # AC5: #3300 comment cites the reworded contract.
    (BRIDGE, r"intentionally not advanced on the owner-scoped path", "3605 AC5: #3300 comment reworded"),
    # AC6: test face + wiring.
    (TEST, r"#3605", "3605 AC6: facade test hosts #3605 ACs"),
    (BUILD, r"check_facade_owner_scope_clock_skip_3605", "3605 AC6: build.py wires the linter"),
)

FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        SVC,
        r"query:eval-core-joint",
        "3604 AC4: no new query:* key",
    ),
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

    for path, pattern, label in FORBIDDEN:
        if re.search(pattern, body(path)) is not None:
            failures.append(label)

    hur = body(HUR)
    facade_pos = hur.find("hard_invalidate_via_facade(const char* name, ReemitReason reason)")
    if facade_pos < 0:
        failures.append("3605 AC1: facade signature not found")
    else:
        nxt = hur.find("\nbool ", facade_pos + 1)
        block = hur[facade_pos : nxt if nxt > facade_pos else facade_pos + 12000]
        # Order: the scope prediction must precede both C bumpers, and the
        # bumpers must sit inside the global branch (after the guard).
        probe_pos = block.find("aura_aot_bump_will_be_owner_scoped()")
        guard_pos = block.find("if (c_clocks_owner_skipped)")
        bridge_pos = block.find("aura_hot_update_bump_bridge_epoch();")
        defuse_pos = block.find("aura_hot_update_bump_defuse_version();")
        table_pos = block.find("aura_aot_bump_func_table_epoch()")
        note_pos = block.find("aura_aot_note_cross_eval_hard_owner_scoped()")
        if probe_pos < 0 or guard_pos < 0 or bridge_pos < 0:
            failures.append("3605 AC1: prediction / guard / bumper anchors missing")
        else:
            if not (probe_pos < guard_pos < bridge_pos < defuse_pos):
                failures.append("3605 AC1: scope prediction must precede the joint C bump")
        if table_pos < 0 or bridge_pos > table_pos:
            failures.append("3605 AC1: C bumpers must precede the table bump (#3150 order)")
        if note_pos < 0 or note_pos > table_pos:
            failures.append("3605 AC1: hard-owner-scoped note must be armed before the table bump")

    bridge = body(BRIDGE)
    probe_sig = bridge.find("int aura_aot_bump_will_be_owner_scoped(void) {")
    if probe_sig < 0:
        failures.append("3605 AC2: probe definition not found")
    else:
        window = bridge[probe_sig : probe_sig + 900]
        if re.search(r"g_cross_eval_epoch_force_bump\s*=[^=]", window):
            failures.append("3605 AC2: probe must not write the force TLS (non-consuming)")
        for needle in (
            "aura_aot_state_map_size()",
            "cross_eval_epoch_throttle_armed()",
            "aura_aot_get_reemit_owner_eval()",
            "aura_aot_get_register_owner_eval()",
        ):
            if needle not in window:
                failures.append(f"3605 AC2: probe input missing: {needle}")

    # AC3: the mark gate sits in the bumper owner branch, before the mark.
    mark_pos = bridge.find("aura_aot_mark_peer_slots_soft_stale(owner);")
    gate_pos = bridge.find("if (!hard_os_pref)")
    if mark_pos < 0 or gate_pos < 0 or not (gate_pos < mark_pos):
        failures.append("3605 AC3: peer-slot mark must sit behind the !hard_os_pref gate")
    else:
        owner_window = bridge[gate_pos : mark_pos + 200]
        if "g_last_table_bump_owner_scoped.store(1" not in owner_window:
            failures.append("3605 AC2: owner branch must record the owner-scoped decision")

    # AC4: the stamp gate skips the core bump + C mirror SET + defuse.
    svc = body(SVC)
    gate = svc.find("if (!os_table_bump)")
    if gate < 0:
        failures.append("3604 AC4: stamp gate not found")
    else:
        window = svc[gate : gate + 400]
        for needle in ("bump_bridge_epoch();", "evaluator_.bump_defuse_version_for_test();"):
            if needle not in window:
                failures.append(f"3605 AC4: global-path dual-write missing inside the gate: {needle}")

    # AC6: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3605-"):
                failures.append(f"3605 AC6: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3605.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3605.cpp",
        REPO_ROOT / "tests" / "compiler" / "test_issue_3605.cpp",
    ):
        if probe.exists():
            failures.append(f"3605 AC6: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3605 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any missing row")
    ap.parse_args()
    failures = run_checks()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"check_facade_owner_scope_clock_skip_3605: {len(failures)} failure(s)")
        return 1
    print("check_facade_owner_scope_clock_skip_3605: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
