#!/usr/bin/env python3
# scripts/check_lockless_allow_mse_3652.py -- Issue #3652 source-cite gate.
#
# AC1: evaluator_eval_flat.cpp carries the bool-returning telemetry mirror
#      deny_macro_opt_out_without_mse(Evaluator&, NodeId) (#3650 precedent —
#      the mutate TU helper is file-local) with the same deny face: capability
#      deny counter + durable SE reason macro-mutate-needs-macro-self-evo +
#      typed hygiene audit + provenance blame; security_event includes cite #3652.
# AC2: every lockless allow arm routes through the mirror — exactly 17
#      deny_macro_opt_out_without_mse(*this, ...) call sites (rebind spine +
#      rebind body-walk + replace-value + tweak-literal + remove-node +
#      insert-child + set-body spine + set-body walk + replace-pattern +
#      replace-subtree + replace-subtree walk + splice + wrap + rename-symbol
#      + move-node + inline-call + inline-call body), each arm cites #3652.
# AC3: every mirror call is sandbox-gated (effect_sandbox_mode() != 0) so
#      Soft/Off stays one load — no MSE scan on the zero-cost face.
# AC4: the public prim #3542 face is untouched — deny_macro_opt_out_without_mse
#      helper + hygiene_protected_error call remain in evaluator_primitives_mutate.cpp.
# AC5: the #3301 batch pre-audit no longer skips on opt-out: walk gate is
#      `if (prod_sandbox)`; the global/batch allow arm routes through the
#      #3542 helper (deny_macro_opt_out_without_mse(ev, node, mev)) and the
#      old skip condition is gone.
# AC6: hygiene:set-allow-macro-mutate! #t is capability-gated under
#      Restricted/Strict via deny_marker_clear_without_mse (#3650 mirror);
#      clearing stays ungated.
# AC7: test wiring — ac3652_1..6 in tests/compiler/test_hygiene_mutate_closed_loop.cpp
#      with runner calls; no tests/**/test_issue_3652.cpp; no docs/design/*3652*;
#      build.py registration + root_check_allowlist append.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

EFL = "src/compiler/evaluator_eval_flat.cpp"
MUT = "src/compiler/evaluator_primitives_mutate.cpp"
CPP = "src/compiler/evaluator_primitives_compile.cpp"
TEST = "tests/compiler/test_hygiene_mutate_closed_loop.cpp"
BUILD = "build.py"
ALLOWLIST = "scripts/coverage/root_check_allowlist.txt"

LINTER = "check_lockless_allow_mse_3652"

MIRROR_DEF = "static bool deny_macro_opt_out_without_mse(Evaluator& ev, aura::ast::NodeId id)"
MIRROR_CALL = "deny_macro_opt_out_without_mse(*this,"
MIRROR_CALLS = 17


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(efl: str, mut: str, cpp: str, test: str, build: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    def count(needle: str, hay: str) -> int:
        return hay.count(needle)

    # AC1 — eval-flat telemetry mirror.
    must(MIRROR_DEF, "AC1 mirror def", efl)
    must("macro-mutate-needs-macro-self-evo", "AC1 SE reason", efl)
    must('#include "core/security_event_wal.hh"', "AC1 SE WAL include", efl)
    must("typed_audit::capture_macro_hygiene_audit", "AC1 hygiene audit", efl)
    must("macro_hygiene_provenance_hits_total", "AC1 provenance blame", efl)

    # AC2 — all 17 lockless allow arms route through the mirror.
    got_calls = count(MIRROR_CALL, efl)
    if got_calls != MIRROR_CALLS:
        fails.append(f"AC2: mirror call sites {got_calls} != {MIRROR_CALLS}")
    for cite in (
        "Issue #3652: the :allow-macro? opt-out arm still requires the",
        "Issue #3652: the allow arm still requires MacroSelfEvo (#3542",
        "Issue #3652: the opt-out arm still requires MacroSelfEvo (#3542",
    ):
        must(cite, "AC2 arm cite", efl)
    if count("Issue #3652", efl) < MIRROR_CALLS:
        fails.append("AC2: eval-flat #3652 cites below gate count")
    for op in (
        "batch :rebind: mutation of MacroIntroduced requires ",
        "batch :rebind: MacroIntroduced body requires MacroSelfEvo ",
        "batch :replace-value: mutation of MacroIntroduced requires ",
        "batch :tweak-literal: mutation of MacroIntroduced requires ",
        "batch :remove-node: mutation of MacroIntroduced requires ",
        "batch :insert-child: mutation of MacroIntroduced requires ",
        "batch :set-body: mutation of MacroIntroduced requires ",
        "batch :set-body: MacroIntroduced body requires MacroSelfEvo ",
        "batch :replace-subtree: mutation of MacroIntroduced requires ",
        "batch :replace-subtree: MacroIntroduced body requires ",
        "batch :splice: mutation of MacroIntroduced requires ",
        "batch :wrap: mutation of MacroIntroduced requires ",
        "batch :rename-symbol: MacroIntroduced site requires MacroSelfEvo ",
        "batch :move-node: mutation of MacroIntroduced requires ",
        "batch :inline-call: mutation of MacroIntroduced requires ",
    ):
        must(op, "AC2 deny message", efl)

    # AC3 — every arm sandbox-gated (Soft/Off one load).
    if count("effect_sandbox_mode() != 0", efl) < MIRROR_CALLS:
        fails.append("AC3: sandbox guards below mirror call count")
    must("Soft/Off: one mode load", "AC3 zero-cost note", efl)

    # AC4 — public prim #3542 face untouched (3-arg helper with MakeErrorVal).
    must(
        "deny_macro_opt_out_without_mse(Evaluator& ev, aura::ast::NodeId id, const MakeErrorVal& mev)",
        "AC4 mutate helper kept",
        mut,
    )
    must("if (auto denied = deny_macro_opt_out_without_mse(ev, id, mev))", "AC4 hygiene_protected call", mut)

    # AC5 — batch pre-audit allow arm routes through the #3542 helper.
    must("deny_macro_opt_out_without_mse(ev, node, mev)", "AC5 batch allow arm", mut)
    must("if (op_opt_out || batch_allow_macro ||", "AC5 opt-out selection", mut)
    must("const bool op_opt_out = parse_allow_macro_opt_out(ev, op_args);", "AC5 per-op parse", mut)
    must(":allow-macro? opt-out requires MacroSelfEvo capability", "AC5 batch merr", mut)
    must_not("!ev.get_allow_macro_mutate() && !batch_allow_macro) {", "AC5 old skip gone", mut)

    # AC6 — set-allow-macro-mutate! capability gate.
    must("deny_marker_clear_without_mse(ev, 0)", "AC6 set-allow gate", cpp)
    must("(hygiene:set-allow-macro-mutate! #t) requires MacroSelfEvo capability", "AC6 merr", cpp)
    must("Issue #3652: arming the global opt-out is itself capability-gated", "AC6 cite", cpp)

    # AC7 — test wiring + no artifacts + registration.
    for ac in (
        "static void ac3652_1_batch_allow_denied_without_mse()",
        "static void ac3652_2_public_prim_still_denied()",
        "static void ac3652_3_mse_grant_batch_allow_ok()",
        "static void ac3652_4_soft_off_unchanged()",
        "static void ac3652_5_set_allow_flag_gate()",
        "static void ac3652_6_source_cite()",
    ):
        must(ac, "AC7 test present", test)
    must("ac3652_1_batch_allow_denied_without_mse();", "AC7 runner wired", test)
    must("check_lockless_allow_mse_3652", "AC7 build.py registration", build)
    must("check_lockless_allow_mse_3652.py", "AC7 allowlist entry", allow)
    for stale in ROOT.glob("docs/design/*3652*"):
        fails.append(f"AC7: forbidden design doc {stale.name}")
    for stale in ROOT.glob("tests/**/test_issue_3652*.cpp"):
        fails.append(f"AC7: forbidden issue test {stale.name}")

    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=f"Issue #3652 source-cite gate ({LINTER})")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for build.py parity")
    args = ap.parse_args()
    efl = _read(EFL)
    mut = _read(MUT)
    cpp = _read(CPP)
    test = _read(TEST)
    build = _read(BUILD)
    allow = _read(ALLOWLIST)
    if args.self_test:
        broken = efl.replace("Issue #3652", "Issue #redacted")
        self_fails = _rows(broken, mut, cpp, test, build, allow)
        if not self_fails:
            print("self-test FAILED: mutation undetected")
            return 2
        print("self-test OK: mutation detected")
        return 0
    fails = _rows(efl, mut, cpp, test, build, allow)
    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}")
        return 1
    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
