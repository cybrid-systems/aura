#!/usr/bin/env python3
# scripts/check_remount_densify_pairing_strip_3612.py -- Issue #3612 source-cite gate.
#
# AC1: force_densify_remap_pairing (evaluator_env.cpp) shares the #3548
#      green-face strip on a pairing fail (closure_remount_ok==false,
#      production/Full gated) — Site A.
# AC2: the Phase-5 pin-contract-fail branch (pairing did not run) shares
#      the same strip — Site B (evaluator_mutation_boundary.cpp).
# AC3: the Soft vacuous-true branch never strips (no extra stores).
# AC4: no new counters / query keys / proof structs — reuses
#      strip_green_face_on_remount_last_zero + existing observables.
# AC5: ac3612 rows live in test_remount_force_deopt.cpp; #3548 JIT ACs
#      unchanged; no docs/design/*3612*, no tests/**/test_issue_3612.cpp.
# AC6: build.py wires this linter.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ENV = "src/compiler/evaluator_env.cpp"
MB = "src/compiler/evaluator_mutation_boundary.cpp"
TEST = "tests/compiler/test_remount_force_deopt.cpp"
BUILD = "build.py"
QUERY = "src/compiler/evaluator_primitives_obs_eval.cpp"
OBS = "src/compiler/observability_metrics.h"

SITE_A = "Issue #3612: a densify pairing fail"
SITE_B = "Issue #3612: pin-contract fail forces"
SOFT_MARK = "Soft / empty densify: vacuous axes"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(env: str, mb: str, test: str, build: str, query: str, obs: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1: Site A — pairing body shares the strip, production-gated.
    must(SITE_A, "AC1 cite", env)
    must("!r.closure_remount_ok", "AC1 fail gate", env)
    must("typed_audit::strip_green_face_on_remount_last_zero()", "AC1 strip call", env)
    must('#include "typed_mutation_audit.h"', "AC1 include", env)

    # AC2: Site B — Phase-5 pin-contract-fail branch shares the strip.
    must(SITE_B, "AC2 cite", mb)
    must("typed_audit::strip_green_face_on_remount_last_zero()", "AC2 strip call", mb)

    # AC3: Soft vacuous branch never strips; Site B precedes it.
    pos_soft = mb.find(SOFT_MARK)
    pos_b = mb.find(SITE_B)
    if pos_soft < 0:
        fails.append("AC3: Soft vacuous branch marker missing")
    elif pos_b < 0 or pos_b > pos_soft:
        fails.append("AC3: Site B strip not gated ahead of the Soft vacuous branch")
    else:
        soft_win = mb[pos_soft : pos_soft + 1500]
        must_not("strip_green_face_on_remount_last_zero", "AC3 Soft branch clean", soft_win)

    # AC4: no new observables — reuse only.
    must_not("3612", "AC4 no new metrics field", obs)
    must_not("schema-3612", "AC4 no new query key", query)
    must("strip_green_face_on_remount_last_zero", "AC4 strip helper reused", env)

    # AC5: test rows present; #3548 JIT ACs unchanged; no invent.
    must("ac3612_1_densify_pairing_fail_strips", "AC5 test AC1", test)
    must("ac3612_4_source_and_linter", "AC5 test source", test)
    must("Issue #3612", "AC5 test cite", test)
    must("ac3548_1_tick_last0_strips_stamper", "AC5 JIT AC retained", test)
    must("ac3548_4_soft_observe_only", "AC5 Soft AC retained", test)
    for rel in ("tests/issues/test_issue_3612.cpp", "tests/compiler/test_issue_3612.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: forbidden {rel} exists (#81967)")
    if any(p.name.find("3612-") >= 0 for p in (ROOT / "docs" / "design").glob("*")):
        fails.append("AC5: forbidden docs/design/*3612-* exists (#1655)")

    # AC6: build.py wiring.
    must("check_remount_densify_pairing_strip_3612", "AC6 build.py wiring", build)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_mb = (
            SITE_B
            + "\ntyped_audit::strip_green_face_on_remount_last_zero()\n"
            + "x" * 1600
            + SOFT_MARK
            + "\nvacuous axes only\n"
        )
        ok_fails = _rows(
            SITE_A
            + "\n!r.closure_remount_ok\ntyped_audit::strip_green_face_on_remount_last_zero()\n"
            + '#include "typed_mutation_audit.h"\n'
            + "strip_green_face_on_remount_last_zero\n",
            sample_mb,
            "ac3612_1_densify_pairing_fail_strips ac3612_4_source_and_linter Issue #3612 "
            "ac3548_1_tick_last0_strips_stamper ac3548_4_soft_observe_only",
            "check_remount_densify_pairing_strip_3612",
            "clean",
            "clean",
        )
        neg_fails = _rows("", "", "", "", "schema-3612", "3612")
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        if len(neg_fails) < 10:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(ENV), _read(MB), _read(TEST), _read(BUILD), _read(QUERY), _read(OBS))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("remount densify pairing strip (#3612) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
