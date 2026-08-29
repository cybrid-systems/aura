#!/usr/bin/env python3
"""Issue #3350: densify success rewrites linear_roots via last_object_remap_.

#2280 / #3249 verify-or-drain: live linear root must not be in
old_addresses (fail-closed) and abort/join unpin leftover. Residual:
densify success never rewrites linear_roots identities, so a
densify-tracked linear object in last_object_remap_ either sticky-fails
verify or leaves a stale registry pointer.

Fix: remap_linear_roots_under_moving(last_object_remap_) in
lifetime_pin.hh, called from live_compact(Moving) after slot/pin remap
and before verify_pins_under_moving_compact. Prefer rewrite when the
object moved. Abort/join still unpin_*. Soft/empty registry 0 extra
atomics. No second pin registry / query key.

Contract:
  AC1 rewrite helper + live_compact call precedes verify
  AC2 Soft/empty 0 extra; #2280/#3023/#3249 retained
  AC3 production densify-tracked linear root rewritten or fail-closed
  AC4 after #3249; no invent / docs/design / g_3350_* / schema-3350

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    lp = _read("src/core/lifetime_pin.hh")
    ixx = _read("src/core/lifetime_pin.ixx")
    arena = _read("src/core/arena.ixx")
    test = _read("tests/compiler/test_linear_pin_moving_compact.cpp")
    moving = _read("tests/core/test_moving_compact.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp") + _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    must("kLinearRootMovingRemapIssue = 3350", "AC1 stamp", lp)
    must("remap_linear_roots_under_moving", "AC1 helper", lp)
    must("remap_linear_roots_under_moving", "AC1 ixx export", ixx)
    must("remap_linear_roots_under_moving", "AC1 live_compact", arena)
    must("old_addrs.erase(new_ptr)", "AC1 packing dest exclude", arena)
    must("ac3350_1", "AC1 test", test)

    rm = arena.find("lifetime::remap_linear_roots_under_moving")
    vf = arena.find("lifetime::verify_pins_under_moving_compact")
    if vf < 0:
        fails.append("AC1: verify_pins_under_moving_compact missing in arena.ixx")
    elif rm < 0 or rm > vf:
        fails.append("AC1: remap_linear_roots_under_moving must precede verify in live_compact")

    must("if (roots.empty() || last_object_remap.empty())", "AC2 empty early-return", lp)
    must("ac3350_2_soft_quiet", "AC2 test", test)
    must("kLinearRootAbortReleaseIssue = 3023", "AC2 #3023 retained", lp)
    must("kLinearNestedAbortDrainIssue = 3249", "AC2 #3249 retained", lp)
    must("this verify never unpins", "AC2 densify never unpins", lp)
    must("unpin_all_linear_roots", "AC2 abort drain", lp)

    must("ac3350_3", "AC3 e2e", moving)
    must(
        "pin_linear_root", "AC3 e2e pins linear root", moving[moving.find("ac3350_3") :] if "ac3350_3" in moving else ""
    )
    if "schema-3350" in q or "schema-3350" in lp:
        fails.append("AC3: new schema-3350 query key")
    if "g_3350_" in lp or "g_3350_" in arena:
        fails.append("AC3: new g_3350_* counter")

    must("check_linear_root_moving_remap_3350", "AC4 build.py", build)
    must("check_linear_nested_abort_drain_3249", "AC4 after #3249", build)
    i3249 = build.find("check_linear_nested_abort_drain_3249.py")
    i3350 = build.find("check_linear_root_moving_remap_3350.py")
    if i3249 < 0 or i3350 < 0 or i3350 < i3249:
        fails.append("AC4: #3350 linter must run after #3249")
    must("ac3350_4_linter_no_invent", "AC4 test", test)
    if (ROOT / "tests" / "issues" / "test_issue_3350.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3350.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3350.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3350.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3350-*")):
            fails.append(f"AC4: docs/design/{f.name}")
    if "AgentRegistry" in lp[lp.find("Issue #3350") : lp.find("Issue #3350") + 1800] if "Issue #3350" in lp else False:
        fails.append("AC4: must not introduce AgentRegistry")

    if fails:
        print("FAIL #3350 linear_root_moving_remap:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3350 linear_root_moving_remap: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
