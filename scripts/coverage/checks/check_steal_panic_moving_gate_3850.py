#!/usr/bin/env python3
"""Issue #3850: steal clears panic defer before CP; Moving omits has_panic_checkpoint.

Residual: steal_complete clears per-eval panic defer at entry; production
clears Evaluator checkpoint later (Soft never). Between those points,
should_defer_destructive_gc() can be false while has_panic_checkpoint()
is still true. compact_sweep OR-gates the checkpoint; Moving densify
entry did not. panic_residual_ok keyed off process panic depth, not the
live Evaluator CP. Post-densify audit is too late.

Contract (one row per AC):
  AC1  Moving densify entry OR-gates evaluator_has_panic_checkpoint_probe
       with should_defer_destructive_gc (align compact_sweep); live CP
       depth independent of defer; Soft leftover observe-only
  AC2  panic_residual_ok = !ev->has_panic_checkpoint() ||
       gc_deferred_for_evaluator; production steal clears CP before
       orphan defer drain; Soft keeps CP
  AC3  Extends test_steal_complete_gc_defer + test_moving_densify_fail_closed;
       dedicated linter + grandfather + manifest + build.py; no invent /
       docs/design (#1655 / #81967)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ARENA = "src/core/arena.ixx"
HOOKS = "src/core/gc_hooks.h"
MB = "src/compiler/evaluator_mutation_boundary.cpp"
EFM = "src/compiler/evaluator_fiber_mutation.cpp"
STEAL = "tests/serve/test_steal_complete_gc_defer.cpp"
FAIL = "tests/core/test_moving_densify_fail_closed.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3850.json"
LINTER = "check_steal_panic_moving_gate_3850"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    arena = _read(ARENA)
    hooks = _read(HOOKS)
    mb = _read(MB)
    efm = _read(EFM)
    steal = _read(STEAL)
    failc = _read(FAIL)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    must("Issue #3850", "AC1 cite", arena)
    must("evaluator_has_panic_checkpoint_probe()", "AC1 Moving probe", arena)
    # Probe appears in the Moving precondition gate near should_defer.
    mov = arena.find("} else if (mode == LiveCompactMode::Moving)")
    if mov < 0:
        fails.append("AC1: Moving mode entry missing")
        mwin = ""
    else:
        mwin = arena[mov : mov + 2500]
    must("should_defer_destructive_gc()", "AC1 defer gate retained", mwin)
    must("evaluator_has_panic_checkpoint_probe()", "AC1 probe in Moving gate", mwin)

    must("g_live_panic_checkpoint_depth", "AC1 live depth", hooks)
    must("evaluator_has_panic_checkpoint_probe", "AC1 probe API", hooks)
    must("note_panic_checkpoint_live", "AC1 note live", hooks)
    must("note_panic_checkpoint_cleared", "AC1 note cleared", hooks)
    must("Issue #3850", "AC1 hooks cite", hooks)
    must_not("g_3850_", "AC1 no invented counter", hooks)
    must_not("g_3850_", "AC1 no invented counter arena", arena)

    must("Issue #2595 / #3850", "AC2 residual cite", mb)
    must("ev_->has_panic_checkpoint()", "AC2 has_cp axis", mb)
    ok_pos = mb.find("panic_residual_ok = !panic_cp_live_now || panic_deferred_now")
    if ok_pos < 0:
        fails.append("AC2: panic_residual_ok formula missing")
    else:
        owin = mb[max(0, ok_pos - 600) : ok_pos + 200]
        must("ev_->has_panic_checkpoint()", "AC2 formula keys off has_cp", owin)
        must_not(
            "g_gc_defer_pending_panic_depth.load",
            "AC2 not depth-driven for live_now",
            owin,
        )

    must("Issue #3850: production clears live PanicCheckpoint BEFORE", "AC2 steal early clear", efm)
    early = efm.find("Issue #3850: production clears live PanicCheckpoint BEFORE")
    defer = efm.find("clear_gc_defer_for_evaluator(prev_eval_id)", early if early >= 0 else 0)
    if not (0 <= early < defer):
        fails.append("AC2: early CP clear must precede clear_gc_defer")
    must("observe-only", "AC2 Soft leftover observe", efm)
    # Soft must not force clear in the early window (production_hard gate).
    if early >= 0:
        ewin = efm[early : early + 900]
        must("production_hard", "AC2 Soft skips early clear", ewin)

    must("ac3850_1_soft_leftover_probe_blocks_moving();", "AC3 steal suite", steal)
    must("ac3850_2_production_clears_cp_before_defer();", "AC3 steal AC2", steal)
    must("ac3850_3_panic_residual_ok_keys_off_has_cp();", "AC3 steal AC3", steal)
    must("ac3850_1_live_cp_blocks_moving();", "AC3 fail_closed suite", failc)
    must("ac3850_2_source_cite_gate_and_residual();", "AC3 fail_closed cite", failc)
    must("ac3850_3_wiring_no_invent();", "AC3 fail_closed wiring", failc)

    must(LINTER, "AC3 build registration", build)
    must("Issue #3850", "AC3 build cite", build)
    must(f"{LINTER}.py", "AC3 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC3 grandfather path", gf)
    must('"issue": 3850', "AC3 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC3: {MANIFEST} missing")
    for rel in (
        "tests/compiler/test_issue_3850.cpp",
        "tests/core/test_issue_3850.cpp",
        "tests/serve/test_issue_3850.cpp",
        "tests/issues/test_issue_3850.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC3: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3850*"):
            fails.append(f"AC3: docs/design/{p.name} exists")

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3850", file=sys.stderr)
        return 1

    print(
        f"OK {LINTER}: Moving OR-gates live CP probe; panic_residual_ok "
        "keys off has_panic_checkpoint; production steal clears CP before defer"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
