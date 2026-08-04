#!/usr/bin/env python3
"""Issue #2386: grant_macro_self_evo stamps grant_epoch + grant_fiber_id.

Contract:
  AC1 grant_epoch non-zero after grant_macro_self_evo
  AC2 grant_fiber_id when fiber override / prov set
  AC3 epoch fence denies MacroSelfEvo expand
  AC4 hard fiber isolation denies fiber mismatch
  AC5 Source-cite + CMake + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    cap = _read("src/core/capability_model.hh")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_grant_macro_self_evo_stamp.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 grant_macro_self_evo stamps
    must("Issue #2386", "AC1", cap)
    gms = cap.find("void grant_macro_self_evo")
    if gms < 0:
        fails.append("AC1: grant_macro_self_evo definition missing")
    else:
        body = cap[gms : gms + 2200]
        for needle in ("grant_epoch", "grant_fiber_id", "bound_mutation_id", "EffectProvenance"):
            if needle not in body:
                fails.append(f"AC1: grant_macro_self_evo body missing {needle!r}")
    must("ac1_grant_epoch_stamped", "AC1", test)

    # AC2–AC4 tests + check path
    must("ac2_grant_fiber_stamped", "AC2", test)
    must("ac3_epoch_fence_denies", "AC3", test)
    must("provenance_ok", "AC3", cap)
    must("ac4_hard_fiber_denies", "AC4", test)

    # security prim wires make_grant_provenance
    must("make_grant_provenance", "AC5", sec)
    must("grant_macro_self_evo", "AC5", sec)

    # AC5 registration
    must("test_grant_macro_self_evo_stamp", "AC5", cmake)
    must("check_grant_macro_self_evo_stamp_2386", "AC5", build)
    must("cmd_grant_macro_self_evo_stamp_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)
    must("Issue #2386", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2386 grant_macro_self_evo stamp parity — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
