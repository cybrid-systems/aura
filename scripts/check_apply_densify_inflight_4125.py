#!/usr/bin/env python3
# scripts/check_apply_densify_inflight_4125.py -- Issue #4125 gate.
#
# AC1: the shared #3421/#3948 refuse predicate
#      (production_apply_closure_densify_hard_refuse) consults
#      densify_in_flight_for(eval_id) inside the production gate and BEFORE
#      the #4006 seq-skip, citing #4125; the extern "C" wrapper still
#      forwards to the shared face and the JIT native-dispatch call site is
#      unchanged (no new query key, no second model).
# AC2: Soft/Off zero extra — the consult sits after the
#      production_defaults_active() gate return (single gate/pref load on
#      Soft) and no schema-4125 / g_4125_ counter is invented.
# AC3: try_acquire / try_acquire_for_region and steal_safety consult the
#      eval-keyed slot DIRECTLY and are behavior-unchanged
#      ("AdmissionRejected: densify-in-flight" intact; steal victim
#      hard-AND densify_in_flight_for(victim_eval_id) intact).
# AC4: test wiring — the five ac4125_* ACs run in
#      run_test_moving_densify_fail_closed before the Results line; the
#      extern "C" refuse face is declared in the test TU; the peer-worker
#      soak drives it; no docs/design/4125-*; no tests/**/test_issue_4125*.
# AC5: build.py registration + root_check_allowlist.txt append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

APPLY = "src/compiler/evaluator_eval_flat.cpp"
JIT = "src/compiler/aura_jit_runtime.cpp"
MUTATE = "src/compiler/evaluator_mutation_boundary.cpp"
STEAL = "src/serve/steal_safety.cpp"
TEST = "tests/core/test_moving_densify_fail_closed.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_apply_densify_inflight_4125"

PREDICATE_HEAD = "static bool production_apply_closure_densify_hard_refuse(ast::ASTArena* arena, const Closure& cl,"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(apply: str, jit: str, mutate: str, steal: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — shared-predicate consult, gate-first, before the seq-skip.
    gate = apply.find(PREDICATE_HEAD)
    if gate < 0:
        fails.append("AC1: shared predicate not located")
        return fails
    body = apply[gate : gate + 2000]
    must("Issue #4125", "AC1 rationale cites the window", body)
    must("aura::core::densify_consistency::densify_in_flight_for(eval_id)", "AC1 consult", body)
    gpos = body.find("production_defaults_active()")
    ipos = body.find("aura::core::densify_consistency::densify_in_flight_for(eval_id)")
    spos = body.find("densify_refuse_seq_skip")
    if gpos < 0 or ipos < 0 or not gpos < ipos:
        fails.append("AC1: consult must sit after the production gate return")
    if spos < 0 or not ipos < spos:
        fails.append("AC1: consult must sit before the #4006 seq-skip")
    must(
        "return true;",
        "AC1 probe refuses",
        body[body.find("densify_in_flight_for(eval_id)") : body.find("densify_in_flight_for(eval_id)") + 80]
        if ipos >= 0
        else body,
    )
    must(
        "production_apply_closure_densify_hard_refuse(nullptr, cl, eval_id)",
        "AC1 extern-C wrapper forwards to the shared face",
        apply,
    )
    must(
        "aura_production_densify_stale_refuse(aura_current_eval_identity())",
        "AC1 JIT native-dispatch call site unchanged",
        jit,
    )
    must(
        '#include "core/densify_consistency_report.h"',
        "AC1 header include present",
        apply,
    )

    # AC2 — no new query key / counter anywhere on the touched surface.
    for hay, name in ((apply, "eval_flat"), (jit, "jit_runtime"), (mutate, "mutation_boundary")):
        must_not("schema-4125", f"AC2 no new query key ({name})", hay)
        must_not("g_4125_", f"AC2 no invented counter ({name})", hay)

    # AC3 — admit + steal consult the slot directly, unchanged.
    must("AdmissionRejected: densify-in-flight", "AC3 admit refuse reason", mutate)
    must("Issue #3956: densify-in-flight refuses new outermost mutate", "AC3 try_acquire cite", mutate)
    must(
        "Issue #3956: same densify-in-flight admit refuse as try_acquire",
        "AC3 try_acquire_for_region cite",
        mutate,
    )
    count = mutate.count(
        "typed_audit::production_defaults_active() &&\n        aura::core::densify_consistency::densify_in_flight_for(static_cast<const void*>(&ev))"
    )
    if count < 2:
        fails.append(f"AC3: expected both direct admit probes, found {count}")
    must("densify_in_flight_for(victim_eval_id)", "AC3 steal victim hard-AND", steal)
    must("held-clear is BoundarySafe via densify_in_flight_for", "AC3 #4033 steal comment", steal)

    # AC4 — test wiring before the Results line; peer soak drives the face.
    must(
        'extern "C" int aura_production_densify_stale_refuse(void* eval_id) noexcept;',
        "AC4 extern-C face declared in the test TU",
        test,
    )
    for ac in (
        "ac4125_apply_dispatch_inflight_refuse();",
        "ac4125_soft_off_zero_extra();",
        "ac4125_admit_steal_unchanged();",
        "ac4125_peer_worker_soak();",
        "ac4125_source_cite_and_wiring();",
    ):
        must(ac, "AC4 runner wired", test)
    must("=== Issue #4125: apply/JIT consult densify-in-flight", "AC4 runner header", test)
    hdr = test.find("=== Issue #4125: apply/JIT consult densify-in-flight")
    res = test.find("=== Results: ")
    if hdr < 0 or res < 0 or not hdr < res:
        fails.append("AC4: #4125 block must run before the Results line")
    must("aura_production_densify_stale_refuse(&ev)", "AC4 soak drives the refuse face", test)
    must("std::thread peer([", "AC4 peer-worker soak", test)
    for stale in ROOT.glob("docs/design/*4125*"):
        fails.append(f"AC4: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_4125*.cpp"):
        fails.append(f"AC4: forbidden issue test {stale.name}")

    # AC5 — build.py registration + allowlist append.
    must("check_apply_densify_inflight_4125.py", "AC5 build.py registration", build)
    must(LINTER, "AC5 allowlist append", allow)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #4125 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    apply = _read(APPLY)
    jit = _read(JIT)
    mutate = _read(MUTATE)
    steal = _read(STEAL)
    test = _read(TEST)
    build = _read(BUILD)
    allow = _read(ALLOWLIST)
    if args.self_test:
        broken = apply.replace(
            "aura::core::densify_consistency::densify_in_flight_for(eval_id)",
            "densify_redacted_4125_",
        )
        self_fails = _rows(broken, jit, mutate, steal, test, build, allow)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(apply, jit, mutate, steal, test, build, allow)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
