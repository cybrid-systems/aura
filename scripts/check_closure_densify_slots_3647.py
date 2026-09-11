#!/usr/bin/env python3
# scripts/check_closure_densify_slots_3647.py -- Issue #3647 source-cite gate.
#
# AC1: Evaluator::register_known_moving_densify_root_slots walks closures_
#      and pushes &cl.flat / &cl.pool as lasting void** slots; registration
#      stays on the existing register_external_root_slot_for_densify_all
#      SSOT; additive counter g_moving_closure_slots_registered_total with
#      issue stamp 3647 + read/reset helpers in densify_consistency_report.h.
# AC2: No canary dual-note on closure slots (#3368) — comment-stripped
#      register-helper body contains no note_post_moving_live_ptr_canary_all
#      call; the temporary inventory drain stays the only canary injection.
# AC3: Moving-window-only gating — Phase-5 flush calls the register helper
#      inside moving_compact_enabled(); the arena known-roots hook fires
#      inside the production auto-arm Moving arm; recovery densify stays
#      behind moving_compact_enabled().
# AC4: Lock discipline — the closures walk collects under shared
#      closures_mtx_ and the walk precedes slot registration (no arena lock
#      under closures_mtx_); no new pin/GC API in the walk.
# AC5: Suite wiring — ac3647_1..5 defined and called in
#      tests/core/test_moving_densify_fail_closed.cpp; no
#      tests/**/test_issue_3647.cpp; no docs/design/3647*; build.py
#      registration + root_check_allowlist append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MB = "src/compiler/evaluator_mutation_boundary.cpp"
HDR = "src/core/densify_consistency_report.h"
ARENA = "src/core/arena.ixx"
TEST = "tests/core/test_moving_densify_fail_closed.cpp"
BUILD = "build.py"

LINTER = "check_closure_densify_slots_3647"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _strip_line_comments(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines():
        cmt = line.find("//")
        out.append(line if cmt < 0 else line[:cmt])
    return "\n".join(out)


def _rows(mb: str, hdr: str, arena_src: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    def order(first: str, second: str, label: str, hay: str) -> None:
        i, j = hay.find(first), hay.find(second)
        if i < 0 or j < 0 or i >= j:
            fails.append(f"{label}: expected {first!r} before {second!r}")

    # AC1 — closure body slots enter the known-root inventory.
    must("Issue #3647", "AC1 walk cite", mb)
    must("for (auto& [cid, cl] : closures_)", "AC1 closures walk", mb)
    must(
        "known_slots.push_back(reinterpret_cast<void**>(&cl.flat));",
        "AC1 flat slot",
        mb,
    )
    must(
        "known_slots.push_back(reinterpret_cast<void**>(&cl.pool));",
        "AC1 pool slot",
        mb,
    )
    must("g_moving_closure_slots_registered_total.fetch_add(", "AC1 counter bump", mb)
    must(
        "register_external_root_slot_for_densify_all(slot)",
        "AC1 slot SSOT",
        mb,
    )
    must("g_moving_closure_slots_registered_total{0}", "AC1 counter declared", hdr)
    must("kMovingClosureSlotsIssue = 3647", "AC1 issue stamp", hdr)
    must("moving_closure_slots_registered_total_v_read", "AC1 read helper", hdr)
    must("reset_moving_closure_slots_registered_for_test", "AC1 reset helper", hdr)

    # AC2 — slot XOR canary (#3368).
    begin = mb.find("std::size_t Evaluator::register_known_moving_densify_root_slots()")
    end = mb.find("recover_moving_sticky_densify_off", begin + 1)
    if begin < 0 or end < 0:
        fails.append("AC2: register helper body not located")
    else:
        body_code = _strip_line_comments(mb[begin:end])
        must_not("note_post_moving_live_ptr_canary_all(", "AC2 dual-note call", body_code)
        must(
            "note_temporary_moving_live_canaries_all()",
            "AC2 temp drain present",
            body_code,
        )

    # AC3 — Moving-window-only gating.
    order(
        "if (aura::ast::moving_compact_enabled())",
        "ev_->register_known_moving_densify_root_slots();",
        "AC3 phase5 gate",
        mb,
    )
    order(
        "should_production_auto_arm_moving(frag_before)",
        "invoke_known_roots_hook();",
        "AC3 hook in auto-arm",
        arena_src,
    )
    must("live_compact(LiveCompactMode::Moving)", "AC3 hook arm requests Moving", arena_src)
    must(
        "if (retry_densify && arena_group_ && aura::ast::moving_compact_enabled())",
        "AC3 recovery gate",
        mb,
    )

    # AC4 — lock discipline + no new pin/GC in the walk.
    if begin < 0 or end < 0:
        fails.append("AC4: register helper body not located")
    else:
        raw_body = mb[begin:end]
        lock = raw_body.find("std::shared_lock<std::shared_mutex> rlock(closures_mtx_)")
        walk = raw_body.find("for (auto& [cid, cl] : closures_)")
        reg = raw_body.find("register_external_root_slot_for_densify_all(slot)")
        if lock < 0 or walk < 0 or reg < 0 or not (lock < walk < reg):
            fails.append("AC4: expected shared closures_mtx_ lock before walk before registration")
        code4 = _strip_line_comments(raw_body)
        must_not("pin_", "AC4 no pin API", code4)
        must_not("gc_", "AC4 no gc API", code4)

    # AC5 — suite wiring + forbidden artifacts.
    for ac in (
        "ac3647_1_closure_body_slots_registered();",
        "ac3647_2_slot_rewrite_green_and_value_only_red();",
        "ac3647_3_soft_zero_work_and_gating();",
        "ac3647_4_no_pin_no_dual_note();",
        "ac3647_5_soak_windows_and_wiring();",
    ):
        must(ac, "AC5 runner wired", test)
    must("ClosureBodySlots3647", "AC5 runtime struct", test)
    must_not("test_issue_3647", "AC5 no tests/issues file", test)
    for stale in ROOT.glob("docs/design/*3647*"):
        fails.append(f"AC5: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_3647*.cpp"):
        fails.append(f"AC5: forbidden issue test {stale.name}")
    must("check_closure_densify_slots_3647.py", "AC5 build.py registration", build)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #3647 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    mb = _read(MB)
    hdr = _read(HDR)
    arena_src = _read(ARENA)
    test = _read(TEST)
    build = _read(BUILD)
    if args.self_test:
        broken = mb.replace("Issue #3647", "Issue #redacted")
        self_fails = _rows(broken, hdr, arena_src, test, build)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(mb, hdr, arena_src, test, build)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
