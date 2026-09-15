#!/usr/bin/env python3
"""Issue #3831: Production dirty-aware storm force-full cap (cone amortize).

Under Production, production_dirty_aware_storm_force_full used to rewrite
DirtyAware masks to all-1s + mark_all_blocks_dirty on SoA while Global was
held — O(module) every suite round. Cap with a second dirty_n threshold
(kDefaultPartialRelowerThreshold) while Global held; storm-exit cooldown
(#3070 Shape+deopt) still force-full at dirty_n>0. Soft consult-only;
compact still never feeds storm ring.

Contract:
  AC1 HighMutation / injected storm N rounds: cone skips accumulate;
      force-full not permanent all-dirty
  AC2 Between storm enter/exit, sparse dirty-cone skips can be non-zero
  AC3 Soft consult-only unchanged; compact never feeds storm ring;
      suite / stamp / build wiring; no invent test_issue_*.cpp; no docs/design/

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

    core = _read("src/compiler/pass_pipeline_core.ixx")
    svc = _read("src/compiler/service.ixx")
    impls = _read("src/compiler/pass_impls.ixx")
    test = _read("tests/compiler/test_partial_relower_storm_gate.cpp")
    shape = _read("src/compiler/shape_profiler.cpp")
    build = _read("build.py")

    must("kDirtyAwareStormForceFullCapIssue = 3831", "AC1 stamp", core)
    must("Issue #3831", "AC1 cite", core)
    must("kDefaultPartialRelowerThreshold", "AC1 second threshold", core)
    must("storm_level_has_global()", "AC1 Global gate", core)

    fn = core.find("production_dirty_aware_storm_force_full")
    if fn < 0:
        fails.append("AC1: production_dirty_aware_storm_force_full missing")
        body = ""
    else:
        nxt = core.find("\nstatic_assert", fn)
        body = core[fn : nxt if nxt > fn else fn + 900]
    must("dirty_n < kDefaultPartialRelowerThreshold", "AC1 sparse cap", body)
    must("return false", "AC1 sparse false", body)

    must("Issue #3831", "AC1 suite cite", svc)
    must("production_dirty_aware_storm_force_full", "AC1 suite helper", svc)
    must("Issue #3831", "AC1 hot pack cite", impls)

    must("3831 AC2", "AC2 test", test)
    must("dirty-cone skips non-zero while Global held", "AC2 soak", test)
    must("3831 AC1/AC2: cone skips accumulate", "AC1 N-round", test)
    must("sparse-under-Global keeps cone", "AC2 sparse", test)

    must("3831 AC3: Soft consult-only", "AC3 Soft", test)
    must("last_storm_from_compact_", "AC3 compact flag", shape)
    must("deopt_storm_compact_suppressed", "AC3 compact suppress", shape)
    must("check_dirty_aware_storm_force_full_cap_3831", "AC3 build.py", build)

    if "schema-3831" in core or "schema-3831" in svc:
        fails.append("AC3: new schema-3831 query key")
    if (ROOT / "tests" / "compiler" / "test_issue_3831.cpp").is_file():
        fails.append("AC3: forbidden tests/compiler/test_issue_3831.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3831.cpp").is_file():
        fails.append("AC3: forbidden tests/issues/test_issue_3831.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3831-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3831 dirty_aware_storm_force_full_cap:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3831 dirty_aware_storm_force_full_cap: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
