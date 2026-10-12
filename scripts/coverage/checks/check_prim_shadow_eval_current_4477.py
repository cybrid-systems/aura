#!/usr/bin/env python3
"""Issue #4477: user define / module export shadowing a primitive must
survive set-code + eval-current.

#4273 dropped EVERY top_ binding whose name is a registered primitive — once
inside set-code and again after every eval-current walk — so the first reload
silently rerouted (check ...) call sites to the registered primitive (printed
1 instead of the user-check line). aura-kernel-sink saw the same face with a
user-module export after the workspace was seeded.

Fix: set-code no longer drops bindings (it swaps the workspace source only);
eval-current scopes the #4273 shadow drop to prim-named top-level Define
heads in the walked workspace (ws_prim_defines). Pre-existing user defines
and module exports keep shadowing; a prim-name define inside the reloaded
workspace still falls through to the prim (#4273 AC23 parity kept).

Contract (one row per AC):
  AC1  src/compiler/evaluator_primitives_eval.cpp cites #4477; eval-current
       scopes the drop (ws_prim_defines + lookup_primitive +
       unbind_local); the set-code blanket drop is gone; #4273 cites kept
  AC2  extends tests/compiler/test_primcall_narg.cpp AC24/AC25/AC26
       (user define face, module export face, source gate), invoked
       + file-mode suite tests/suite/prim_shadow_eval_current_4477.aura
       (AC1 user define, AC2 module export, AC3 #4273 parity)
  AC3  linter + grandfather + manifest + build.py; no invent /
       docs/design; #4273 gate stays textual-green

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = "src/compiler/evaluator_primitives_eval.cpp"
TEST = "tests/compiler/test_primcall_narg.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/4477.json"
LINTER = "check_prim_shadow_eval_current_4477"


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

    src = _read(SRC)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    # ── AC1: scoped shadow drop in eval-current; set-code drops nothing ──
    must("Issue #4477", "AC1 cite", src)
    must("ws_prim_defines", "AC1 workspace-define scan", src)
    must("lookup_primitive", "AC1 prim lookup", src)
    must("unbind_local", "AC1 unbind", src)
    must_not("Drop bindings whose names are registered primitives", "AC1 blanket drop gone", src)
    # #4273 parity citations must stay (gate check_setcode_pick_parity_4272_4273 AC4).
    must("Issue #4273", "AC1 #4273 cite kept", src)

    # ── AC2: aligned suite ACs present + invoked ──
    must("ac24_user_define_prim_shadow_4477", "AC2 AC24 fn", test)
    must("ac24_user_define_prim_shadow_4477();", "AC2 AC24 invoked", test)
    must("ac25_module_export_prim_shadow_4477", "AC2 AC25 fn", test)
    must("ac25_module_export_prim_shadow_4477();", "AC2 AC25 invoked", test)
    must("ac26_source_gate_4477", "AC2 AC26 fn", test)
    must("ac26_source_gate_4477();", "AC2 AC26 invoked", test)
    must("#4477", "AC2 cite", test)

    # ── AC2b: file-mode suite (the issue repro face) ──
    suite = _read("tests/suite/prim_shadow_eval_current_4477.aura")
    must(
        '(define (check name ok) (display (list "user-check" name ok)) (newline) 42)', "AC2b issue repro define", suite
    )
    must('(assert! (= (check "after" #t) 42)', "AC2b user define after reload", suite)
    must('(assert! (= (check= "mod2" #t) 43)', "AC2b module export after reload", suite)
    must("(assert! (> 3 1)", "AC2b #4273 parity kept", suite)
    must("OK-4477", "AC2b ok sentinel", suite)

    # ── AC3: wiring + no invent ──
    must(LINTER, "AC3 build.py", build)
    must("Issue #4477", "AC3 build cite", build)
    must(f"{LINTER}.py", "AC3 grandfather", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC3 grandfather path", gf)
    must('"issue": 4477', "AC3 manifest", man)

    if (ROOT / "tests/compiler/test_issue_4477.cpp").is_file():
        fails.append("AC3: test_issue_4477.cpp invent forbidden")
    if list((ROOT / "docs/design").glob("4477-*")):
        fails.append("AC3: docs/design/4477-* invent forbidden")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: #4477 prim shadow survives set-code + eval-current — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
