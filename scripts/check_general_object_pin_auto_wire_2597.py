#!/usr/bin/env python3
"""Issue #2597: auto-wire GeneralObjectPin for all densify-tracked
intermediate creates (production default AURA_GENERAL_OBJECT_PIN=required).

Coverage gate: presence-checks for the production-default lock wiring in
apply_production_security_defaults (src/compiler/security_defaults.hh)
plus env-override branches and lifetime_pin.ixx source-cite. Mirrors
`check_moving_untracked_production_hard_2596.py` /
`check_densify_unified_gate_2595.py` style.

Contract:
  AC16 production default locks pref=1 (required) when production active
       (sandbox != off) AND env unset
  AC17 AURA_GENERAL_OBJECT_PIN=off under production keeps Soft
       (operator override — AC3 explicit off wins)
  AC18 Soft / AURA_SANDBOX=off + env unset keeps observe-only
       (pref stays at default -1)
  AC19 apply_general_object_pin_required_env handles required / off
       env values (linter source-cite + call site in security_defaults)
  AC20 GENERAL_OBJECT_PIN_EXEMPT marker source-cite in lifetime_pin.ixx
       (for sites that don't need a wire call — stable handle /
       RootRemap-registered only)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/security_defaults.hh")
    lp = _read("src/core/lifetime_pin.ixx")
    test = _read("tests/core/test_general_object_pin_coverage_gate_2496.cpp")
    build = _read("build.py")

    # AC16: production default locks pref=1 under production.
    must("Issue #2597", "AC16", hh)
    must("AURA_GENERAL_OBJECT_PIN=required", "AC16", hh)
    must("apply_general_object_pin_required_env", "AC16", hh)
    must(
        "g_general_object_pin_required_pref.store(1, std::memory_order_release)",
        "AC16",
        hh,
    )
    must(
        "g_general_object_pin_required_pref.load(std::memory_order_relaxed) == -1",
        "AC16",
        hh,
    )
    must("!dev_off", "AC16", hh)

    # AC17: explicit env=off overrides production lock.
    must("apply_general_object_pin_required_env", "AC17", lp)
    must(
        "g_general_object_pin_required_pref.store(0, std::memory_order_release)",
        "AC17",
        lp,
    )
    must('"off"', "AC17", lp)

    # AC18: Soft + env unset keeps observe-only.
    must("dev_off = sandbox_e", "AC18", hh)
    must("!dev_off", "AC18", hh)
    must("g_general_object_pin_required_pref{-1}", "AC18", lp)

    # AC19: env parser source-cite.
    must("apply_general_object_pin_required_env", "AC19", lp)
    must("AURA_GENERAL_OBJECT_PIN", "AC19", lp)
    must("required", "AC19", lp)
    must('"off"', "AC19", lp)

    # AC20: GENERAL_OBJECT_PIN_EXEMPT marker source-cite.
    must("GENERAL_OBJECT_PIN_EXEMPT", "AC20", lp)
    must("stable-handle", "AC20", lp)
    must("RootRemap-registered", "AC20", lp)

    # Test additions (per #81967 — same src-aligned test file as #2496).
    must("Issue #2597", "test", test)
    must("ac16_production_default_required", "test", test)
    must("ac17_env_off_operator_override", "test", test)
    must("ac18_soft_unset_keeps_observe", "test", test)
    must("ac19_env_parser_source_cite", "test", test)
    must("ac20_exempt_marker_source_cite", "test", test)
    must(
        "production default AURA_GENERAL_OBJECT_PIN=required (extends #2496 test file per #81967)",
        "test",
        test,
    )
    must("AURA_GENERAL_OBJECT_PIN", "test", test)

    # build.py wiring.
    must("cmd_general_object_pin_auto_wire_2597_coverage", "build", build)
    must("check_general_object_pin_auto_wire_2597", "build", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\n{len(fails)} general-object-pin-auto-wire (#2597) contract row(s) failed",
            file=sys.stderr,
        )
        return 1
    print("OK: Issue #2597 general object pin auto wire — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
