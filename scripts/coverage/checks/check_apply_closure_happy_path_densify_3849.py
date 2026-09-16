#!/usr/bin/env python3
"""Issue #3849: happy-path apply_closure must densify-hard-refuse before eval_flat.

Residual: production densify-stale refuse (#3421) was wired only on
MustDeopt / safe_fallback / race arms. Bridge-epoch-green happy path
went straight to eval_flat with no window / LCP / remap consult → UAF
under densify-old flat* (composes with #3848 objects_moved==0).

Contract (one row per AC):
  AC1  Happy path calls production_apply_closure_densify_hard_refuse
       before bridge_epoch_hit / eval_flat; note helper; Soft/Off early
       false retained; no invent / second pin registry
  AC2  Extends densify-stale (test_setcode_rebind_survive ac17) +
       moving densify fail-closed (ac3849_*); Soft happy-path apply retained
  AC3  Dedicated linter + grandfather + manifest + build.py; no invent /
       docs/design (#1655 / #81967)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FLAT = "src/compiler/evaluator_eval_flat.cpp"
SURVIVE = "tests/compiler/test_setcode_rebind_survive.cpp"
FAIL = "tests/core/test_moving_densify_fail_closed.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3849.json"
LINTER = "check_apply_closure_happy_path_densify_3849"


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

    flat = _read(FLAT)
    survive = _read(SURVIVE)
    failc = _read(FAIL)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    must("Issue #3849", "AC1 cite", flat)
    hit = flat.find("metrics->bridge_epoch_hit_count_.fetch_add")
    if hit < 0:
        fails.append("AC1: bridge_epoch_hit site missing")
        hwin = ""
    else:
        hwin = flat[max(0, hit - 700) : hit + 120]
    must("production_apply_closure_densify_hard_refuse", "AC1 happy-path helper", hwin)
    must("note_apply_closure_densify_hard_refuse", "AC1 note helper", hwin)
    must("Issue #3849", "AC1 happy-path cite", hwin)
    # Refuse precedes the hit bump (no eval_flat on densify-old).
    if hwin:
        refuse_pos = hwin.find("production_apply_closure_densify_hard_refuse")
        hit_rel = hwin.find("bridge_epoch_hit_count_")
        if not (0 <= refuse_pos < hit_rel):
            fails.append("AC1: densify refuse must precede bridge_epoch_hit / eval_flat")

    begin = flat.find("static bool production_apply_closure_densify_hard_refuse")
    end = flat.find("static void note_apply_closure_densify_hard_refuse", begin)
    arm = flat[begin:end] if begin >= 0 and end > begin else ""
    must("production_defaults_active()", "AC1 Soft early-false", arm)
    must("Soft / Off never take this refuse", "AC1 Soft contract", flat)
    must_not("g_3849_", "AC1 no invented counter", flat)
    must_not("class DensifyClosurePinRegistry", "AC1 no second pin registry", flat)
    must_not("g_moving_pin_registry_3849", "AC1 no second pin registry", flat)

    calls = flat.count("production_apply_closure_densify_hard_refuse(")
    if calls < 5:  # 1 definition + 4 apply sites
        fails.append(f"AC1: expected >=4 apply sites, found {max(calls - 1, 0)}")

    must("ac17_3849_happy_path_densify_refuse();", "AC2 densify-stale suite", survive)
    must("happy-path densify-old flat* hard-refuses", "AC2 refuse AC", survive)
    must("3849 Soft: happy-path apply unchanged", "AC2 Soft AC", survive)
    must("ac3849_1_happy_path_refuse_source_cite();", "AC2 fail_closed suite", failc)
    must("ac3849_2_wiring_no_invent();", "AC2 wiring AC", failc)

    must(LINTER, "AC3 build registration", build)
    must("Issue #3849", "AC3 build cite", build)
    must(f"{LINTER}.py", "AC3 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC3 grandfather path", gf)
    must('"issue": 3849', "AC3 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC3: {MANIFEST} missing")
    for rel in (
        "tests/compiler/test_issue_3849.cpp",
        "tests/core/test_issue_3849.cpp",
        "tests/issues/test_issue_3849.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC3: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3849*"):
            fails.append(f"AC3: docs/design/{p.name} exists")

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3849", file=sys.stderr)
        return 1

    print(f"OK {LINTER}: happy-path apply_closure densify refuse before eval_flat")
    return 0


if __name__ == "__main__":
    sys.exit(main())
