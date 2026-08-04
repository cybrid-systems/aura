#!/usr/bin/env python3
"""Issue #2298: general (non-render) object pin-or-remap coverage linter.

  AC1: pin_or_fail / GeneralObjectPin + validate after Moving densify
  AC2: validate fail-closed (missing pin / gen mismatch counters)
  AC3: Render / PinOwner path retained (PresentGuard / PinOwner)
  AC4: Soft path zero remap when no densify
  AC5: Object-class inventory + query keys + schema-2298 + tests

  Issue #2337: GeneralObjectPin adoption in mutate/agent create paths.
  AC6: wire-up counter on LifetimePinStats (per-call-site that adopts
       GeneralObjectPin in mutate create paths).
  AC7: 4 new query keys on query:compact-stats + schema/issue sentinels.
  AC8: wire-up site in evaluator_primitives_mutate.cpp + import +
       GeneralObjectPin::pin calls + counter bump.
  AC9: tests/core/test_general_object_pin_2298.cpp extended with
       ac6_2337 / ac7_2337 / ac8_2337 functions + Issue #2337 cite.

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LP = ROOT / "src" / "core" / "lifetime_pin.ixx"
Q = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
M = ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp"
TEST = ROOT / "tests" / "core" / "test_general_object_pin_2298.cpp"
CMAKE = ROOT / "CMakeLists.txt"


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    lp = LP.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    m = M.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")
    cmake = CMAKE.read_text(encoding="utf-8", errors="replace")

    # AC1: pin API + remap-aware validate
    must("pin_or_fail", "AC1", lp)
    must("class GeneralObjectPin", "AC1", lp)
    must("validate_general_object", "AC1", lp)
    must("general_object_pin_total", "AC1", lp)
    must("general_object_pin_remap_ok_total", "AC1", lp)

    # AC2: fail-closed counters
    must("general_object_pin_validate_fail_total", "AC2", lp)
    must("AC2: unpinned validate fails", "AC2", test)
    must("AC2: gen mismatch fails validate", "AC2", test)

    # AC3: PinOwner / render path retained
    must("enum class PinOwner", "AC3", lp)
    must("Render / FFI present buffers", "AC3", lp)
    must("AC3: default Arena owner", "AC3", test)

    # AC4: Soft zero remap
    must("Soft/Force do not relocate", "AC4", lp)
    must("AC4: Soft does not densify", "AC4", test)
    must("AC4: Soft + live pin → zero remap", "AC4", test)

    # AC5: inventory + query + schema + CMake
    must("Object class × required protocol inventory", "AC5", lp)
    must("Intermediate general buffers", "AC5", lp)
    must("kGeneralObjectPinIssue = 2298", "AC5", lp)
    must("general-object-pin-total", "AC5", q)
    must("general-object-pin-validate-fail-total", "AC5", q)
    must("general-object-pin-remap-ok-total", "AC5", q)
    must("general-object-pin-wired", "AC5", q)
    must("schema-2298", "AC5", q)
    must("issue-2298", "AC5", q)
    must("test_general_object_pin_2298", "AC5", cmake)
    must("void ac1_pin_or_remap_after_moving", "AC5", test)
    must("void ac2_missing_pin_fail_closed", "AC5", test)
    must("void ac4_soft_zero_cost", "AC5", test)
    must("void ac5_inventory_and_surface", "AC5", test)

    # Issue #2337: GeneralObjectPin adoption in mutate/agent create paths.
    # AC6: wire-up counter on LifetimePinStats struct.
    # AC7: 4 new query keys on query:compact-stats + schema/issue sentinels.
    # AC8: wire-up site in evaluator_primitives_mutate.cpp.
    # AC9: tests extended with ac6_2337 / ac7_2337 / ac8_2337 functions.
    must("general_object_pin_mutate_wire_total", "AC6", lp)
    must("Issue #2337", "AC6", lp)
    must("adoption wire-up counter", "AC6", lp)
    must("general-object-pin-mutate-wire-total", "AC7", q)
    must("general_object_pin_mutate_wire_total", "AC7", q)
    must("general-object-pin-mutate-wired", "AC7", q)
    must("schema-2337", "AC7", q)
    must("issue-2337", "AC7", q)
    must("Issue #2337", "AC7", q)
    must("import aura.core.lifetime_pin", "AC8", m)
    # Issue #2363: mutate site uses wire_general_object_create_pair (two pins).
    must("wire_general_object_create_pair", "AC8", m)
    must("pat_pool_pin", "AC8", m)
    must("pat_flat_pin", "AC8", m)
    must("void ac6_2337_wire_counter_initialized", "AC9", test)
    must("void ac7_2337_schema_sentinels", "AC9", test)
    must("void ac8_2337_source_cite", "AC9", test)
    must("Issue #2337 (Refine #2298)", "AC9", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: general object pin-or-remap (#2298) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
