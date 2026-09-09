#!/usr/bin/env python3
# scripts/check_remount_reason_domain_3607.py -- Issue #3607 source-cite gate.
#
# AC1: the covered walk filters per-closure by the #3229 define side set
#      (relower_success_covers_define) and falls back to the full named FIFO
#      walk when the side set is idle; the sid%64 helper is deleted — the
#      #3445 reason word is not a sid bitmap.
# AC2: last_reemit_success stays the reason-group stamp (#3445/#3466); the
#      remount walks never restamp it with the full demoted mask (#3413).
# AC3: the residual tick prefer pass gates on production + non-idle reason
#      word + define side set active; idle side set walks FIFO (pre-#2977).
# AC4: #3229 fail-close rows preserved (hh collision guard + covered skip).
# AC5: zero-extra gates preserved (mask==0||cap==0, production probe).
# AC6: build.py wires this linter; ac3607_* rows live in the residual suite;
#      no docs/design/*3607*, no tests/**/test_issue_3607.cpp.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

RT = "src/compiler/aura_jit_runtime.cpp"
REG = "src/compiler/hot_update_registry.cpp"
HH = "src/compiler/hot_update_registry.hh"
TEST = "tests/compiler/test_anonymous_residual_stable_id_policy.cpp"
BUILD = "build.py"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    _ = ap.parse_args()

    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    rt = _read(RT)
    reg = _read(REG)
    hh = _read(HH)
    test = _read(TEST)
    build = _read(BUILD)

    # AC1
    must("Issue #3607", "AC1 runtime", rt)
    must_not("residual_closure_sid_region_bits_unlocked", "AC1 runtime", rt)
    must("relower_success_covers_define", "AC1 runtime filter", rt)
    must("full named FIFO", "AC1 runtime idle fallback", rt)
    must("ac3607_1_covered_define_domain", "AC1 test row", test)

    # AC2
    must("aot_reload_fail_to_force_jit_mask(fail) & demoted", "AC2 reason stamp", reg)
    must("last_reemit_success is a reason-group bitmap", "AC2 #3445 comment", reg)
    must("it is NOT a sid", "AC2 domain comment", rt)
    must("ac3607_2_reason_domain_untouched", "AC2 test row", test)

    # AC3
    must("prefer_define", "AC3 prefer gate", rt)
    must("aura_hot_update_relower_success_define_active", "AC3 define-active gate", rt)
    must("pre-#2977", "AC3 idle FIFO fallback", rt)
    must("ac3607_3_prefer_define_not_sid_decoy", "AC3 test row", test)

    # AC4
    must("!relower_success_covers_define(id)", "AC4 hh collision guard", hh)
    must("unrecorded id stays residual", "AC4 hh fail-close doc", hh)
    must("unrecorded id stays residual", "AC4 runtime fail-close", rt)
    must("ac3607_4_fail_close_unrecorded", "AC4 test row", test)

    # AC5
    must("mask == 0 || cap == 0", "AC5 zero-walk gate", rt)
    must("production_defaults_active()", "AC5 production gate", rt)
    must("aura_production_defaults_active_probe", "AC5 registry probe", reg)
    must("ac3607_5_idle_zero_extra", "AC5 test row", test)

    # AC6
    must("check_remount_reason_domain_3607", "AC6 build wiring", build)
    must("ac3607_6_source_and_linter", "AC6 test row", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3607.cpp").is_file():
        fails.append("AC6: test_issue_3607.cpp present (forbidden per #81934/#81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3607*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3607 covered/prefer remount reason domain — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
