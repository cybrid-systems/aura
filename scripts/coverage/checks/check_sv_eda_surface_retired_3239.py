#!/usr/bin/env python3
"""Issue #3239: fully retire residual EDA / SystemVerilog (SV) mutate surface.

Contract (one row per AC):
  AC1  mutate:sv-add-coverpoint / mutate:sv-weaken-property not registered
       and not listed in gen_docs.py
  AC2  no kSvaDirty, no sv_mutate_* counters, no maybe_sv_hardware_closedloop
  AC3  3218 SV hygiene linter deleted; 3191 rewritten off the SV prims
  AC4  tests extend hygiene_mutate_closed_loop + mutate_batch; no invent
  AC5  no live production add_mutate of the two SV prims
  AC6  this linter wired into build.py; no docs/design/3239-*

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    ast = _read("src/core/ast.ixx")
    impl = _read("src/core/ast_impl.cpp")
    hw = _read("src/compiler/hardware_backend_impl.cpp")
    gendocs = _read("scripts/tools/gen_docs.py")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    batch = _read("tests/compiler/test_mutate_batch.cpp")
    build = _read("build.py")
    l3191 = _read("scripts/coverage/checks/check_macro_hygiene_default_deny_3191.py")

    # AC1
    must("Issue #3239", "AC1 mutate cite", mut)
    must_not('add_mutate("mutate:sv-add-coverpoint"', "AC1 coverpoint", mut)
    must_not('add_mutate("mutate:sv-weaken-property"', "AC1 weaken", mut)
    must_not("mutate:sv-add-coverpoint", "AC1 gen_docs coverpoint", gendocs)
    must_not("mutate:sv-weaken-property", "AC1 gen_docs weaken", gendocs)
    must("3239 AC1", "AC1 test", test)

    # AC2
    must_not("kSvaDirty", "AC2 ast kSvaDirty", ast)
    must_not("sv_mutate_attempts_total_", "AC2 attempts", ast)
    must_not("sv_mutate_success_total_", "AC2 success", ast)
    must_not("maybe_sv_hardware_closedloop", "AC2 closedloop", mut)
    must_not("verify_sva_dirty_total_", "AC2 sva dirty counter", ast)
    must("Issue #3239", "AC2 ast_impl cite", impl)
    must("kSvaDirty retired", "AC2 hardware cite", hw)
    must("3239 AC2", "AC2 test", test)

    # AC3
    if (ROOT / "scripts" / "coverage" / "checks" / "check_sv_hygiene_merr_surface_3218.py").is_file():
        fails.append("AC3: check_sv_hygiene_merr_surface_3218.py still present")
    must_not("sv-add-coverpoint cannot", "AC3 3191 rewritten", l3191)
    must("3239 AC3", "AC3 test", test)

    # AC4
    must("ac3239_1_sv_prims_gone", "AC4 test AC1", test)
    must("run_1704_sv_guard", "AC4 mutate_batch still calls retired-check", batch)
    must("3239: mutate:sv-add-coverpoint not registered", "AC4 batch cite", batch)

    # AC5 — production mutate TU has no live SV prim registration.
    must_not('add_mutate("mutate:sv-', "AC5 no sv add_mutate", mut)

    # AC6
    must("check_sv_eda_surface_retired_3239", "AC6 build.py", build)
    must("3239 AC6", "AC6 test", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3239.cpp").is_file():
        fails.append("AC6: test_issue_3239.cpp present (forbidden #81967)")
    if _read("docs/design/3239-retire-sv-eda.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3239 sv_eda_surface_retired:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3239 sv_eda_surface_retired: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
