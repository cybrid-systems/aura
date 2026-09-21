#!/usr/bin/env python3
# scripts/check_jit_densify_remap_3972.py -- Issue #3972 source-cite gate.
#
# AC1: the JIT refuse entry surfaces the this-window remap arm — a helper in
#      aura_jit_runtime.cpp consults the #2297 densify object_remap mirror
#      (same map instance the remount rewrite matches; no second densify
#      model, no invented g_3972_* counter, no docs/design, no tests/issues).
# AC2: dispatch prologue ordering — inside the production gate, the #3948
#      window/LCP refuse runs first, then the #3972 remap arm (window → LCP
#      → remap, same order as TW apply_closure); not keyed on JIT table
#      epoch.
# AC3: the remap-hit arm takes the same leave-native refuse as #3948 (stale
#      deopt + safe fallback + deopt + return 0) and the TW helper ABI is
#      unchanged (production_apply_closure_densify_hard_refuse(nullptr, cl,
#      eval_id) stays — no churn to #3948's pinned shape).
# AC4: test wiring — ac20_3972_native_dispatch_remap_arm wired into
#      run_test_setcode_rebind_survive after ac19; live ACs drive the real
#      mirror (aura_set_densify_object_remap) + dispatch +
#      aura_jit_closure_stale_deopt_total; no test_issue_3972 file.
# AC5: build.py registration + root_check_allowlist.txt append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

JIT = "src/compiler/aura_jit_runtime.cpp"
FLAT = "src/compiler/evaluator_eval_flat.cpp"
TEST = "tests/compiler/test_setcode_rebind_survive.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_jit_densify_remap_3972"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(jit: str, flat: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — JIT remap-arm helper reuses the #2297 mirror (single model).
    must("Issue #3972", "AC1 cite", jit)
    must("closure_env_cells_hit_window_remap_", "AC1 helper", jit)
    must("densify_remap_detail::object_remap()", "AC1 #2297 mirror consult", jit)
    must("Issue #2297", "AC1 mirror provenance cite", jit)
    must_not("g_3972_", "AC1 no invented counter", jit)
    must_not("std::unordered_map<void*, void*> g_3972", "AC1 no second remap map", jit)
    for stale in ROOT.glob("docs/design/*3972*"):
        fails.append(f"AC1: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_3972*.cpp"):
        fails.append(f"AC1: forbidden issue test {stale.name}")

    # AC2 — prologue ordering: production gate → #3948 refuse → #3972 remap.
    begin = jit.find("int64_t aura_closure_dispatch_native_checked(")
    if begin < 0:
        fails.append("AC2: native dispatch entry not located")
        return fails
    cite3948 = jit.find("Issue #3948: same densify-stale refuse", begin)
    cite3972 = jit.find("Issue #3972", begin)
    if cite3948 < 0 or cite3972 < 0:
        fails.append("AC2: refuse cites not located in the prologue")
        return fails
    if cite3948 >= cite3972:
        fails.append("AC2: remap arm must follow the #3948 window/LCP refuse")
    prod_gate = jit.rfind("production_defaults_active()", begin, cite3948)
    if prod_gate < 0:
        fails.append("AC2: production gate before refuses")
        prologue = jit[begin:cite3972]
    else:
        prologue = jit[prod_gate:cite3972]
    must("production_defaults_active()", "AC2 production gate before refuses", prologue)
    if "g_closure_table_epochs" in jit[cite3948 : cite3972 + 400]:
        fails.append("AC2: refuse must not be keyed on JIT table epoch")

    # AC3 — same leave-native arm; TW helper ABI unchanged.
    arm = jit[cite3972 : cite3972 + 1400]
    must("closure_env_cells_hit_window_remap_(", "AC3 dispatch consults the arm", arm)
    must("aura_jit_closure_record_stale_deopt", "AC3 stale deopt recorded", arm)
    must("aura_jit_closure_record_safe_fallback", "AC3 safe fallback recorded", arm)
    must("aura_deopt_inc()", "AC3 deopt bump", arm)
    must(
        "production_apply_closure_densify_hard_refuse(nullptr, cl, eval_id)",
        "AC3 TW helper ABI unchanged",
        flat,
    )
    must("resolve_object_remap(static_cast<void*>(cl.flat))", "AC3 TW remap arm kept", flat)
    must("resolve_object_remap(static_cast<void*>(cl.pool))", "AC3 TW remap arm kept", flat)

    # AC4 — test wiring + live mirror-driven ACs.
    must("ac20_3972_native_dispatch_remap_arm();", "AC4 runner wired", test)
    must("ac19_3946_native_moving_canary();", "AC4 runner anchor", test)
    if test.find("ac20_3972_native_dispatch_remap_arm();") < test.find("ac19_3946_native_moving_canary();"):
        fails.append("AC4: ac20 must run after ac19")
    must("aura_set_densify_object_remap(olds, news, 1)", "AC4 live mirror publish", test)
    must("aura_deopt_count()", "AC4 deopt-counter observable", test)
    must_not("aura_jit_closure_stale_deopt_total()", "AC4 no vacuous metrics assertion", test)
    must("--- #3972: native dispatch surfaces this-window remap arm ---", "AC4 AC header", test)
    must("Issue #3972", "AC4 test cite", test)

    # AC5 — build.py registration + allowlist append.
    must("check_jit_densify_remap_3972.py", "AC5 build.py registration", build)
    must(LINTER, "AC5 allowlist append", allow)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #3972 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    jit = _read(JIT)
    flat = _read(FLAT)
    test = _read(TEST)
    build = _read(BUILD)
    allow = _read(ALLOWLIST)
    if args.self_test:
        broken = jit.replace("closure_env_cells_hit_window_remap_", "closure_redacted_")
        self_fails = _rows(broken, flat, test, build, allow)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(jit, flat, test, build, allow)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
