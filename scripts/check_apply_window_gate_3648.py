#!/usr/bin/env python3
# scripts/check_apply_window_gate_3648.py -- Issue #3648 source-cite gate.
#
# AC1: the closure apply arm consults the densify window gate
#      (window_would_allow_mutate over the g_last_* axes) before the LCP
#      consult; cites #3648. #3848: no objects_moved==0 early return.
# AC2: the #3421 half-guards remain — LCP consult + resolve_object_remap on
#      flat/pool still refuse after the window gate (no regression).
# AC3: Soft / Off never refuse (production gate); empty-remap fast-path
#      (#3848) keeps true-zero quiet recover; #2569 recover surface kept.
# AC4: counters reused — the note helper still bumps closure_stale_returns +
#      compiler_root_dangling_prevented; no invented g_3648_* counter.
# AC5: the FFI arm carries the same gate; test wiring (ac9..ac12 + #3848
#      ac16 + #3849 ac17 in tests/compiler/test_setcode_rebind_survive.cpp
#      with the extended ProdDensifyWindowGuard); no tests/**/test_issue_3648.cpp;
#      no docs/design/3648*; build.py registration + allowlist append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FLAT = "src/compiler/evaluator_eval_flat.cpp"
TEST = "tests/compiler/test_setcode_rebind_survive.cpp"
BUILD = "build.py"

LINTER = "check_apply_window_gate_3648"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(flat: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — closure arm window-gate consult.
    begin = flat.find("static bool production_apply_closure_densify_hard_refuse")
    end = flat.find("static void note_apply_closure_densify_hard_refuse", begin)
    if begin < 0 or end < 0 or end <= begin:
        fails.append("AC1: closure arm not located")
        return fails
    arm = flat[begin:end]
    must("Issue #3648", "AC1 cite", arm)
    must("window_would_allow_mutate", "AC1 gate consult", arm)
    must("g_last_untracked_kept", "AC1 untracked axis", arm)
    must("g_last_had_moving_densify", "AC1 had axis", arm)
    must("g_last_pin_contract_held", "AC1 pin axis", arm)
    must("g_last_root_remap_fail_total", "AC1 root-fail axis", arm)
    gate_pos = arm.find("window_would_allow_mutate")
    lcp_pos = arm.find("last_lifetime_consistency_would_allow")
    remap_size_pos = arm.find("object_remap_size")
    if "g_last_objects_moved" in arm:
        fails.append("AC1: objects_moved==0 early return must be gone (#3848)")
    if gate_pos < 0 or lcp_pos < 0 or gate_pos >= lcp_pos:
        fails.append("AC1: window consult must precede the LCP consult")
    if remap_size_pos < 0 or gate_pos >= remap_size_pos:
        fails.append("AC1: empty-remap fast-path (#3848) must follow window")

    # AC2 — #3421 half-guards remain after the gate.
    must("resolve_object_remap", "AC2 remap half-guard", arm)
    must("last_lifetime_consistency_would_allow", "AC2 LCP half-guard", arm)
    must("resolve_object_remap(static_cast<void*>(cl.flat))", "AC2 flat resolve", arm)
    must("resolve_object_remap(static_cast<void*>(cl.pool))", "AC2 pool resolve", arm)

    # AC3 — Soft/Off never refuse; empty-remap fast-path keeps true-zero quiet.
    must("if (!aura::compiler::typed_audit::production_defaults_active())", "AC3 prod gate", arm)
    must("kApplyClosureDensifyHardRefuseIssue = 3421", "AC3 issue stamp kept", flat)
    must("Soft / Off never take this refuse", "AC3 Soft contract", flat)
    must("Issue #3848", "AC3 #3848 cite", flat)

    # AC4 — counters reused; no invented counter.
    must_not("g_3648_", "AC4 no invented counter", flat)
    must("closure_stale_returns.fetch_add", "AC4 stale-returns bump kept", flat)
    must("bump_compiler_root_dangling_prevented()", "AC4 dangling-prevented bump kept", flat)

    # AC5 — FFI arm + test wiring + forbidden artifacts.
    ffi_begin = flat.find("production_ffi_apply_densify_hard_refuse")
    ffi_end = flat.find("// Issue #1511", ffi_begin)
    if ffi_begin < 0 or ffi_end < 0 or ffi_end <= ffi_begin:
        fails.append("AC5: FFI arm not located")
    else:
        ffi_body = flat[ffi_begin:ffi_end]
        must("Issue #3648", "AC5 FFI cite", ffi_body)
        must("window_would_allow_mutate", "AC5 FFI gate", ffi_body)
        must("g_last_untracked_kept", "AC5 FFI untracked axis", ffi_body)
    for ac in (
        "ac9_3648_window_gate_refuse();",
        "ac10_3648_green_window_remap_still_refuse();",
        "ac11_3648_soft_no_move_recover();",
        "ac12_3648_wiring_and_family();",
        "ac16_3848_zero_move_publish_still_refuse();",
        "ac17_3849_happy_path_densify_refuse();",
    ):
        must(ac, "AC5 runner wired", test)
    must("std::uint64_t untracked = 0, std::uint64_t root_fail = 0", "AC5 guard axes", test)
    must("=== #2569/#3421/#3469/#3602/#3634/#3648/#3848/#3849:", "AC5 summary line", test)
    must_not("test_issue_3648", "AC5 no tests/issues file", test)
    must("check_apply_window_gate_3648.py", "AC5 build.py registration", build)
    for stale in ROOT.glob("docs/design/*3648*"):
        fails.append(f"AC5: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_3648*.cpp"):
        fails.append(f"AC5: forbidden issue test {stale.name}")

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #3648 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    flat = _read(FLAT)
    test = _read(TEST)
    build = _read(BUILD)
    if args.self_test:
        broken = flat.replace("Issue #3648", "Issue #redacted")
        self_fails = _rows(broken, test, build)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(flat, test, build)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
