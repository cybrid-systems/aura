#!/usr/bin/env python3
# scripts/check_steal_densify_inflight_boundary_safe_4033.py -- Issue #4033.
#
# Densify-in-flight BoundarySafe is composition-only (#3860/#3894):
# evaluate_residual_hard_and_bits ANDs densify_in_flight_for into the
# BoundarySafe arm (no dedicated StealInvariant bit). Tip production doors
# are closed; CI must fail-closed if a future edit restores a bare
# is_at_mutation_boundary_safe(snap) BoundarySafe probe.
#
# AC1: tip composition still present — BoundarySafe arm of
#      evaluate_residual_hard_and_bits calls densify_in_flight_for( after
#      is_at_mutation_boundary_safe(snap).
# AC2: stripping densify_in_flight_for from that arm → this check fails
#      (--self-test mutates the TU text).
# AC3: Soft LCP Soft≠vuln unchanged — LifetimeProofOk Soft skip cite +
#      hard/latch gate stay; no second StealInvariant enum invented.
# AC4: build.py registration + root_check_allowlist.txt append; no
#      docs/design/4033-*; no tests/**/test_issue_4033.cpp.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SS = "src/serve/steal_safety.cpp"
SS_H = "src/serve/steal_safety.h"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_steal_densify_inflight_boundary_safe_4033"

FN_SIG = "evaluate_residual_hard_and_bits(Fiber* stolen"
BOUNDARY_ARM = "if (!skip(StealInvariant::BoundarySafe))"
SNAP_PROBE = "is_at_mutation_boundary_safe(snap)"
DENSIFY_PROBE = "densify_in_flight_for("
SOFT_SKIP = "Soft: skip entirely (no loads)"
LCP_GATE = (
    "if (!skip(StealInvariant::LifetimeProofOk) &&\n"
    "        (is_steal_snapshot_hard_mode() || aura_runtime_multi_worker_production_latched() != 0))"
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _boundary_arm_window(ss: str) -> str:
    """Return the BoundarySafe arm body inside evaluate_residual_hard_and_bits."""
    fn = ss.find(FN_SIG)
    if fn < 0:
        return ""
    # Stop at the next StealInvariant arm or function end.
    arm = ss.find(BOUNDARY_ARM, fn)
    if arm < 0:
        return ""
    nxt = ss.find("// StealInvariant::LayoutStampMatch", arm)
    if nxt < 0:
        nxt = ss.find("StealInvariant::LayoutStampMatch", arm)
    if nxt < 0:
        nxt = arm + 1200
    return ss[arm:nxt]


def _rows(ss: str, ss_h: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — tip composition: densify_in_flight_for AFTER snap probe in arm.
    must("Issue #3894", "AC1 #3894 cite", ss)
    must("Issue #3860", "AC1 #3860 cite", ss)
    must(FN_SIG, "AC1 evaluate_residual_hard_and_bits", ss)
    arm = _boundary_arm_window(ss)
    if not arm:
        fails.append("AC1: BoundarySafe arm not located in evaluate_residual_hard_and_bits")
        return fails
    must(SNAP_PROBE, "AC1 snap BoundarySafe probe", arm)
    must(DENSIFY_PROBE, "AC1 densify_in_flight_for AND", arm)
    snap_at = arm.find(SNAP_PROBE)
    densify_at = arm.find(DENSIFY_PROBE)
    if snap_at < 0 or densify_at < 0 or densify_at < snap_at:
        fails.append(
            "AC1: densify_in_flight_for( must appear after "
            "is_at_mutation_boundary_safe(snap) in BoundarySafe arm"
        )
    must("aura_fiber_evaluator_id_for_steal_safety(stolen)", "AC1 victim-eval id", arm)
    # Composition-only: still OR'd into BoundarySafe fail_bits (no new bit).
    must(
        "fail_bits |= steal_invariant_mask(StealInvariant::BoundarySafe)",
        "AC1 BoundarySafe fail_bits",
        arm,
    )

    # AC3 — Soft LCP Soft≠vuln unchanged; no second StealInvariant enum.
    must(SOFT_SKIP, "AC3 Soft Lifetime skip cite", ss)
    must("StealInvariant::LifetimeProofOk", "AC3 LifetimeProofOk arm", ss)
    must(
        "is_steal_snapshot_hard_mode() || aura_runtime_multi_worker_production_latched() != 0",
        "AC3 Lifetime hard/latch gate (Soft≠vuln)",
        ss,
    )
    must_not("StealInvariant::DensifyInFlight", "AC3 no second StealInvariant enum", ss)
    must_not("StealInvariant::DensifyInFlight", "AC3 no second StealInvariant enum", ss_h)
    must_not("DensifyInFlight =", "AC3 no DensifyInFlight enum value", ss_h)
    must_not("g_4033_", "AC3 no invented counter", ss)
    must_not("schema-4033", "AC3 no new query key", ss)

    for stale in ROOT.glob("docs/design/*4033*"):
        fails.append(f"AC4: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_4033*.cpp"):
        fails.append(f"AC4: forbidden issue test {stale.name}")

    # AC4 — wiring.
    must(LINTER + ".py", "AC4 build.py registration", build)
    must(LINTER, "AC4 allowlist append", allow)

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #4033 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()

    ss = _read(SS)
    ss_h = _read(SS_H)
    build = _read(BUILD)
    allow = _read(ALLOWLIST)

    if args.self_test:
        # AC2: stripping densify_in_flight_for from the BoundarySafe arm
        # must be detected. Mutate only inside the arm window so other
        # densify_in_flight_for cites (comments / other TUs) do not mask.
        arm = _boundary_arm_window(ss)
        if not arm or DENSIFY_PROBE not in arm:
            print("self-test FAILED: tip arm missing densify_in_flight_for (cannot mutate)")
            return 2
        broken_arm = arm.replace(DENSIFY_PROBE, "densify_REDACTED_for(")
        broken_ss = ss.replace(arm, broken_arm, 1)
        self_fails = _rows(broken_ss, ss_h, build, allow)
        if not self_fails:
            print("self-test FAILED: stripping densify_in_flight_for undetected")
            return 2
        # Positive tip must pass once wiring is present; when run before
        # wiring lands, only the mutation-detect half is required.
        tip_fails = _rows(ss, ss_h, build, allow)
        wiring_ok = (LINTER + ".py") in build and LINTER in allow
        if wiring_ok and tip_fails:
            print("self-test FAILED: tip unexpectedly dirty:")
            for f in tip_fails:
                print(f"  {f}")
            return 2
        print("self-test OK: strip densify_in_flight_for → check fails")
        return 0

    fails = _rows(ss, ss_h, build, allow)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
