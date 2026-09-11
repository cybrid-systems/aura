#!/usr/bin/env python3
# scripts/check_rollback_mse_3650.py -- Issue #3650 source-cite gate.
#
# AC1: mutate:rollback-macro-introduced gates the unstamp behind the MSE
#      deny — subtree_has_macro_introduced walker + effect_sandbox_mode
#      scan gate + deny_macro_opt_out_without_mse call textually before
#      aura_unstamp_macro_introduced_with_counter; cites #3650.
# AC2: syntax:set-marker gates only the CLEAR direction (marker_val != 1
#      && is_macro_introduced) via deny_marker_clear_without_mse, after
#      the #3040 tenant gate; stamping MacroIntroduced / User→User keep
#      the existing Mutate/tenant-only gates; cites #3650.
# AC3: both sites share the #3542 contract — require_effect_for_node_id(
#      kEffectMacroSelfEvo, "macro-mutate", id) + SE reason
#      macro-mutate-needs-macro-self-evo.
# AC4: Soft/Off — the scan is gated on effect_sandbox_mode() != 0 (one
#      load, no scan); the #373/#2037 hygiene_protected_error Soft shape
#      and the #2952-era skip contract are untouched.
# AC5: test wiring — ac3650_1..5 in tests/compiler/
#      test_hygiene_mutate_closed_loop.cpp + main wired; no tests/**/
#      test_issue_3650.cpp; no docs/design/3650*; build.py registration
#      + root_check_allowlist append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MUT = "src/compiler/evaluator_primitives_mutate.cpp"
CMP = "src/compiler/evaluator_primitives_compile.cpp"
TEST = "tests/compiler/test_hygiene_mutate_closed_loop.cpp"
BUILD = "build.py"

LINTER = "check_rollback_mse_3650"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(mut: str, cmp_: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — rollback unstamp gated behind the MSE deny.
    must("Issue #3650: subtree would-clear predicate", "AC1 walker cite", mut)
    must("static bool subtree_has_macro_introduced(const aura::ast::FlatAST& flat,", "AC1 walker", mut)
    must("Issue #3650: unstamp IS the macro-mutate face", "AC1 gate cite", mut)
    mut.find("static bool production_apply_closure_densify_hard_refuse")
    # Scope to the rollback prim body — the unstamp helper name also
    # appears in the extern-C declarations at the top of the file.
    prim_pos = mut.find('"mutate:rollback-macro-introduced",')
    # rollback gate must precede the unstamp call inside the prim:
    gate_pos = mut.find("deny_macro_opt_out_without_mse(ev, root, mev)", prim_pos)
    unstamp_pos = mut.find("aura_unstamp_macro_introduced_with_counter(", prim_pos)
    if prim_pos < 0 or gate_pos < 0 or unstamp_pos < 0 or not (gate_pos < unstamp_pos):
        fails.append("AC1: rollback gate must precede the unstamp call")
    scan_pos = mut.find("ev.effect_sandbox_mode() != 0 && subtree_has_macro_introduced(flat, root)", prim_pos)
    if scan_pos < 0 or gate_pos < scan_pos:
        fails.append("AC1: rollback gate scans only under effect_sandbox_mode() != 0")
    must("deny_macro_opt_out_without_mse", "AC1 reuse mutate deny helper", mut)

    # AC2 — set-marker clear-direction gate.
    must("Issue #3650: clearing a MacroIntroduced marker", "AC2 gate cite", cmp_)
    must("deny_marker_clear_without_mse", "AC2 deny helper", cmp_)
    clear_pos = cmp_.find("marker_val != 1 && ev.workspace_flat_->is_macro_introduced(id)")
    tenant_pos = cmp_.find("cross-tenant syntax:set-marker denied (#3040)")
    if clear_pos < 0 or tenant_pos < 0 or not (clear_pos > tenant_pos):
        fails.append("AC2: clear gate must follow the #3040 tenant gate")

    # AC3 — shared #3542 contract at both sites.
    must('require_effect_for_node_id(kEffectMacroSelfEvo, "macro-mutate", id)', "AC3 require", cmp_)
    must('require_effect_for_node_id(kEffectMacroSelfEvo, "macro-mutate", id)', "AC3 require mut", mut)
    must('"macro-mutate-needs-macro-self-evo"', "AC3 SE reason", cmp_)
    must('"macro-mutate-needs-macro-self-evo"', "AC3 SE reason mut", mut)
    # deny helper reuses the #3542 telemetry (deny counter + hygiene blame).
    must("macro_mutate_capability_deny_total", "AC3 deny counter", cmp_)

    # AC4 — Soft/Off one-load shape preserved.
    must("Issue #373 / #2037: MacroIntroduced hygiene guard helper.", "AC4 373 helper intact", mut)
    must("if (ev.effect_sandbox_mode() == 0)", "AC4 soft one-load", mut)
    must_not("test_issue_3650", "AC4 no tests/issues literal", mut)

    # AC5 — test wiring + registration + forbidden artifacts.
    must("static void ac3650_1_rollback_denied_without_mse()", "AC5 AC1", test)
    must("static void ac3650_2_set_marker_clear_denied()", "AC5 AC2", test)
    must("static void ac3650_3_rollback_with_mse_unstamps()", "AC5 AC3", test)
    must("static void ac3650_4_soft_rollback_unchanged()", "AC5 AC4", test)
    must("static void ac3650_5_source_and_no_artifacts()", "AC5 AC5", test)
    must("ac3650_1_rollback_denied_without_mse();", "AC5 main wired", test)
    must("grant_macro_self_evo", "AC5 MSE grant path", test)
    must("check_rollback_mse_3650", "AC5 build.py registration", build)
    for stale in ROOT.glob("docs/design/*3650*"):
        fails.append(f"AC5: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_3650*.cpp"):
        fails.append(f"AC5: forbidden issue test {stale.name}")

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #3650 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    mut = _read(MUT)
    cmp_ = _read(CMP)
    test = _read(TEST)
    build = _read(BUILD)
    if args.self_test:
        broken = mut.replace("Issue #3650", "Issue #redacted")
        self_fails = _rows(broken, cmp_, test, build)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(mut, cmp_, test, build)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
