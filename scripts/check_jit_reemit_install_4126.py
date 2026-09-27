#!/usr/bin/env python3
# scripts/check_jit_reemit_install_4126.py -- Issue #4126 gate.
#
# AC1: note_reemit gates the remount walk inputs on the ScalarFn install —
#      the stable-id push is `if (sid != 0 && installed)` citing #4126 and
#      the old unconditional `if (sid != 0)` guard is gone; only one
#      reemit_stable_ids.push_back remains.
# AC2: the host emit arm passes count_emit_success=installed (mirroring the
#      default-LLVM arm) and no arm passes count_emit_success=true — so the
#      success_count that feeds g_last_reemit_success_count /
#      on_reemit_pipeline_call(to_re_emit, success_count) (last_reemit_success
#      region coverage / force_jit only_covered heal) cannot be healed by a
#      metric-only host true. The pipeline feed call shape is unchanged (no
#      old query key rename, no mid-struct metrics insert).
# AC3: the default-LLVM arm (count_emit_success=installed) and the #1480
#      skeleton arm (count_emit_success=false / installed=false — Soft/Off
#      zero-cost would-reemit unchanged) keep their shapes.
# AC4: the #4100 remap gate composes — window after the #4100 gate comment
#      still orders if (native_installed) → commit_func_table_swap() →
#      aura_remap_live_closures_after_reemit → aura_sync_remount_named_live_
#      closures. Test wiring: the adversarial A-no-install + B-install batch
#      (test_4126_no_install_not_remount_id) is declared and called in the
#      1480 runner before RUN_ALL_TESTS, drives {ac4126a, ac4126b} through
#      the host emit face with a metric-only true for A and a new ScalarFn
#      install for B, and no docs/design/4126-* / tests/**/test_issue_4126*
#      exists.
# AC5: build.py registration + root_check_allowlist.txt append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BRIDGE = "src/compiler/aura_jit_bridge.cpp"
TEST = "tests/compiler/test_jit_aot_hot_update_unit_batch.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_jit_reemit_install_4126"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(bridge: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — the id push is install-gated (#4126), old guard gone.
    must("if (sid != 0 && installed)", "AC1 install-gated id push", bridge)
    must("Issue #4126", "AC1 bridge cite", bridge)
    must_not("if (sid != 0)\n", "AC1 old unconditional id-push guard", bridge)
    if bridge.count("reemit_stable_ids.push_back") != 1:
        fails.append("AC1: reemit_stable_ids.push_back must appear exactly once")
    must("B's install must not remap", "AC1 residual rationale comment", bridge)

    # AC2 — host arm success gated on install; pipeline feed shape intact.
    host_arm = bridge.find("if (g_aot_emit_fn)")
    if host_arm < 0:
        fails.append("AC2: host emit arm missing")
    else:
        host_end = bridge.find("} else if (g_batch_deopt_jit", host_arm)
        if host_end < 0:
            fails.append("AC2: default-LLVM arm anchor missing")
        else:
            win = bridge[host_arm:host_end]
            must("/*count_emit_success=*/installed", "AC2 host arm success gate", win)
            must("#4126", "AC2 host arm residual cite", win)
            must_not("/*count_emit_success=*/true", "AC2 host arm literal true", win)
    must_not("count_emit_success=*/true", "AC2 no arm counts literal true", bridge)
    must(
        "on_reemit_pipeline_call(to_re_emit, success_count)",
        "AC2 pipeline feed unchanged (no key rename)",
        bridge,
    )
    must("g_last_reemit_success_count.store(success_count", "AC2 last-success feed", bridge)

    # AC3 — default-LLVM + skeleton arms unchanged.
    llvm_arm = bridge.find("} else if (g_batch_deopt_jit")
    if llvm_arm < 0:
        fails.append("AC3: default-LLVM arm anchor missing")
    else:
        win = bridge[llvm_arm : llvm_arm + 2600]
        must("/*count_emit_success=*/installed", "AC3 default-LLVM success gate", win)
        must("/*count_llvm=*/false", "AC3 default-LLVM llvm metric", win)
        must("Phase 1 / #1480 skeleton", "AC3 skeleton arm present", win)
        must("/*count_emit_success=*/false", "AC3 skeleton would-reemit only", win)
        must("/*installed=*/false", "AC3 skeleton never installs", win)
        must_not("/*count_emit_success=*/true", "AC3 skeleton literal true", win)

    # AC4 — #4100 remap gate composes; adversarial batch wired in the runner.
    gate = bridge.find("Issue #4100: skeleton and a default-LLVM")
    if gate < 0:
        fails.append("AC4: #4100 gate comment missing")
    else:
        win = bridge[gate : gate + 4000]
        must("if (native_installed)", "AC4 commit gate", win)
        commit = win.find("commit_func_table_swap()")
        remap = win.find("aura_remap_live_closures_after_reemit")
        remount = win.find("aura_sync_remount_named_live_closures")
        if not (0 <= commit < remap < remount):
            fails.append("AC4: commit < remap < remount ordering lost")
    must("bool test_4126_no_install_not_remount_id()", "AC4 test declared", test)
    must("test_4126_no_install_not_remount_id();", "AC4 test called", test)
    runner = test.find("test_4100_no_restamp_without_new_native();")
    call = test.find("test_4126_no_install_not_remount_id();")
    rat = test.find("RUN_ALL_TESTS()", call)
    if not (0 <= runner < call < rat):
        fails.append("AC4: #4126 call must follow #4100 in the 1480 runner before RUN_ALL_TESTS")
    must('feed.candidates = {{"ac4126a", 1, false}, {"ac4126b", 1, false}};', "AC4 adversarial dirty batch {A,B}", test)
    must(
        "static bool emit_fn_4126(const char* name, std::uint64_t /*region*/, void* userdata)",
        "AC4 host emit face",
        test,
    )
    must('std::string_view(name) == "ac4126a"', "AC4 A metric-only true branch", test)
    must(
        'aura_register_fn_named("ac4126b", 4127, ac4126_scalar_b, 4, 1, 0);', "AC4 B new ScalarFn install branch", test
    )
    must("aura_set_aot_emit_fn(&emit_fn_4126, &fx)", "AC4 batch drives the emit face", test)
    must('"4126 AC3: A stays MustDeopt (not in the remount id list)"', "AC4 A-not-remounted assertion", test)
    must('"4126 AC2: A metric-only true does not count as reemit success"', "AC4 success-heal assertion", test)
    must('std::println("\\n--- #4126: A metric-only true + B install dirty batch ---");', "AC4 runner header", test)
    for stale in ROOT.glob("docs/design/*4126*"):
        fails.append(f"AC4: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_4126*.cpp"):
        fails.append(f"AC4: forbidden issue test {stale.name}")

    # AC5 — build.py registration + allowlist append.
    must("check_jit_reemit_install_4126.py", "AC5 build.py registration", build)
    must(LINTER, "AC5 allowlist append", allow)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #4126 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    bridge = _read(BRIDGE)
    test = _read(TEST)
    build = _read(BUILD)
    allow = _read(ALLOWLIST)
    if args.self_test:
        broken = bridge.replace("if (sid != 0 && installed)", "if (sid != 0)")
        self_fails = _rows(broken, test, build, allow)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(bridge, test, build, allow)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
