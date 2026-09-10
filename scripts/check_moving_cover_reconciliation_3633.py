#!/usr/bin/env python3
# scripts/check_moving_cover_reconciliation_3633.py -- Issue #3633 source-cite gate.
#
# Moving densify cover is defined by the create-site call (pin / slot /
# RootRemapPass), not the allocation. The window-exit reconciliation
# fail-closes any relocated object with no cover in any rewrite family.
#
# AC1: Reconciliation runs in live_compact(Moving) BEFORE publish
#      consumption (#3308 LCP proof stamp; #3123 healthy sticky
#      auto-clear must not run over an uncovered window).
# AC2: relocate_tracked_objects_for_moving_ records this-window moved-old
#      addresses (last_moving_relocated_old_) — tombstone-safe denominator.
# AC3: Cover union is membership-by-old-address (slot ∪ pins ∪
#      root-remap); canaries are observe-only (#3017/#3055) — not a family.
# AC4: RootRemapPass records covered old addresses via the thread_local
#      probe (moving_cover_probe.h), cleared at pass start, drained by the
#      arena after the callback.
# AC5: Failure reuses the #2495/#2664 fail face + #2837 sticky densify-off;
#      appended-only schema (uncovered_moved_count /
#      moving_uncovered_relocation_total / g_moving_uncovered_relocation_total).
# AC6: Tests extend the src-aligned suites (tests/core/
#      test_moving_densify_fail_closed.cpp +
#      tests/compiler/test_arena_moving_densify_health.cpp); no
#      docs/design/*3633*, no tests/**/test_issue_3633.cpp; build.py wires
#      this linter.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ARENA = "src/core/arena.ixx"
PASS = "src/compiler/root_remap_pass.ixx"
LP = "src/core/lifetime_pin.hh"
PROBE = "src/core/moving_cover_probe.h"
BUILD = "build.py"
CORE_TEST = "tests/core/test_moving_densify_fail_closed.cpp"
HEALTH_TEST = "tests/compiler/test_arena_moving_densify_health.cpp"

RECON = "Issue #3633: window-exit moved-vs-covered reconciliation"
MOVED_OLD_MEMBER = "std::vector<void*> last_moving_relocated_old_;"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(arena: str, remap: str, probe: str, lp: str, build: str, core_test: str, health_test: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # ── AC1/AC5: reconciliation block + placement + fail face ──
    recon_pos = arena.find("if (result.objects_moved > 0 && !last_moving_relocated_old_.empty()) {")
    if recon_pos == -1:
        fails.append("AC1: reconciliation block missing")
    must(RECON, "AC1", arena)
    proof_pos = arena.find("stamp_lifetime_consistency_proof_for(")
    if proof_pos == -1:
        fails.append("AC1: LCP proof stamp missing (file drift)")
    elif recon_pos != -1 and recon_pos > proof_pos:
        fails.append("AC1: reconciliation must precede the LCP proof stamp")
    must("last_moving_relocated_old_.clear();", "AC1", arena)
    must("result.uncovered_moved_count = uncovered;", "AC5", arena)
    must("g_moving_uncovered_relocation_total.fetch_add(uncovered,", "AC5", arena)
    must("g_moving_incomplete_remap_densify_hard_fail_total.fetch_add(", "AC5", arena)
    must("moving_incomplete_remap = true;", "AC5", arena)
    must("pin_contract_held = false;", "AC5", arena)
    healthy_pos = arena.find("clear_moving_incomplete_remap_sticky_densify_off_reason(reason);")
    if healthy_pos == -1:
        fails.append("AC1: #3123 healthy auto-clear missing (file drift)")
    elif recon_pos != -1 and recon_pos > healthy_pos:
        fails.append("AC1: reconciliation must precede the healthy sticky auto-clear")
    # Sticky reuse gated inside the reconciliation face (not elsewhere-first).
    sticky_pos = arena.find("g_moving_incomplete_remap_sticky_densify_off.exchange", recon_pos)
    if sticky_pos == -1:
        fails.append("AC5: sticky densify-off arm missing in reconciliation face")
    elif proof_pos != -1 and sticky_pos > proof_pos:
        fails.append("AC5: sticky arm must precede the LCP proof stamp")

    # ── AC2: moved-old denominator ──
    must(MOVED_OLD_MEMBER, "AC2", arena)
    must("last_moving_relocated_old_.push_back(p.old);", "AC2", arena)
    must("if (neu != p.old) {", "AC2", arena)

    # ── AC3: membership union (not a family-count sum) ──
    must(
        "const bool covered_anywhere = slot_covered_old.count(moved_old) != 0 ||",
        "AC3",
        arena,
    )
    must("pins_honoring_old.insert(old_ptr);", "AC3", arena)
    must_not("covered += slot_covered_old.size()", "AC3", arena)
    must_not("covered = slots_remapped + remapped_pins", "AC3", arena)
    # Issue #3350/#3633: linear roots are a rewrite channel — a fourth
    # cover family via the remap_linear_roots_under_moving out-param.
    must("std::vector<void*> linear_roots_covered_old;", "AC3", arena)
    must(
        "remap_linear_roots_under_moving(\n                    last_object_remap_, &linear_roots_covered_old);",
        "AC3",
        arena,
    )
    must("covered_old_out->insert(covered_old_out->end(), to_erase.begin(), to_erase.end());", "AC3", lp)

    # ── AC4: probe (record / clear / drain) ──
    must("record_covered_old(it->first);", "AC4", remap)
    must("aura::core::moving_cover_probe::clear();", "AC4", remap)
    must("thread_local", "AC4", probe)
    must("Issue #3633", "AC4", probe)
    must("invoke_root_remap_callback_(result, &root_remap_covered_old)", "AC4", arena)
    must("*covered_old_out = aura::core::moving_cover_probe::drain();", "AC4", arena)

    # ── AC5: appended schema ──
    must("std::size_t uncovered_moved_count = 0;", "AC5", arena)
    must("export inline std::atomic<std::uint64_t> g_moving_uncovered_relocation_total{0};", "AC5", arena)
    must("std::size_t moving_uncovered_relocation_total = 0;", "AC5", arena)

    # ── AC6: src-aligned tests + build wiring; no invent files ──
    must("ac3633_2_unregistered_alias_fail_closed", "AC6", core_test)
    must("ac3633_6_hard_face_sticky_block", "AC6", core_test)
    must("Issue #3633", "AC6", core_test)
    must("ac3633_health_probe_and_publish", "AC6", health_test)
    must("check_moving_cover_reconciliation_3633", "AC6", build)
    must_not("docs/design/3633", "AC6", build)
    p_inv = ROOT / "tests" / "core" / "test_issue_3633.cpp"
    if p_inv.is_file():
        fails.append("AC6: tests/core/test_issue_3633.cpp must not exist (extend src-aligned suite)")

    return fails


def _self_test() -> int:
    """Verify the checker logic on synthetic fixtures (must-pass + must-fail)."""
    good_arena = (
        "// Issue #3633: window-exit moved-vs-covered reconciliation\n"
        "std::vector<void*> last_moving_relocated_old_;\n"
        "last_moving_relocated_old_.push_back(p.old);\n"
        "if (neu != p.old) {\n"
        "const bool covered_anywhere = slot_covered_old.count(moved_old) != 0 ||\n"
        "pins_honoring_old.insert(old_ptr);\n"
        "invoke_root_remap_callback_(result, &root_remap_covered_old)\n"
        "*covered_old_out = aura::core::moving_cover_probe::drain();\n"
        "result.uncovered_moved_count = uncovered;\n"
        "g_moving_uncovered_relocation_total.fetch_add(uncovered,\n"
        "g_moving_incomplete_remap_densify_hard_fail_total.fetch_add(\n"
        "moving_incomplete_remap = true;\n"
        "pin_contract_held = false;\n"
        "last_moving_relocated_old_.clear();\n"
        "export inline std::atomic<std::uint64_t> g_moving_uncovered_relocation_total{0};\n"
        "std::size_t uncovered_moved_count = 0;\n"
        "std::size_t moving_uncovered_relocation_total = 0;\n"
        "g_moving_incomplete_remap_sticky_densify_off.exchange(\n"
        "stamp_lifetime_consistency_proof_for(\n"
        "clear_moving_incomplete_remap_sticky_densify_off_reason(reason);\n"
    )
    good_remap = "record_covered_old(it->first);\naura::core::moving_cover_probe::clear();\n"
    good_probe = "thread_local\nIssue #3633\n"
    good_build = "check_moving_cover_reconciliation_3633\n"
    good_core = "ac3633_2_unregistered_alias_fail_closed\nac3633_6_hard_face_sticky_block\nIssue #3633\n"
    good_health = "ac3633_health_probe_and_publish\n"

    good_lp = (
        "std::vector<void*>* covered_old_out = nullptr) noexcept {\n"
        "covered_old_out->insert(covered_old_out->end(), to_erase.begin(), to_erase.end());\n"
    )
    good_arena = (
        "std::vector<void*> linear_roots_covered_old;\n"
        "remap_linear_roots_under_moving(\n                    last_object_remap_, &linear_roots_covered_old);\n"
        "if (result.objects_moved > 0 && !last_moving_relocated_old_.empty()) {\n" + good_arena
    )
    ok_rows = _rows(good_arena, good_remap, good_probe, good_lp, good_build, good_core, good_health)
    if ok_rows:
        print("self-test FAIL: clean fixture reported errors:")
        for f in ok_rows:
            print(f"  - {f}")
        return 1

    bad_arena = good_arena.replace("const bool covered_anywhere = slot_covered_old.count(moved_old) != 0 ||\n", "")
    bad_rows = _rows(bad_arena, good_remap, good_probe, good_lp, good_build, good_core, good_health)
    if not any("covered_anywhere" in f for f in bad_rows):
        print("self-test FAIL: removed union row not detected")
        return 1

    print("self-test OK (clean fixture passes; union-row removal detected)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3633 moved-vs-covered reconciliation gate")
    ap.add_argument("--strict", action="store_true", help="fail on any missing row")
    ap.add_argument("--self-test", action="store_true", help="verify checker logic on fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    fails = _rows(
        _read(ARENA),
        _read(PASS),
        _read(PROBE),
        _read(LP),
        _read(BUILD),
        _read(CORE_TEST),
        _read(HEALTH_TEST),
    )
    if fails:
        print(f"check_moving_cover_reconciliation_3633: {len(fails)} row(s) failed")
        for f in fails:
            print(f"  ✗ {f}")
        return 1
    print("check_moving_cover_reconciliation_3633: all rows OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
