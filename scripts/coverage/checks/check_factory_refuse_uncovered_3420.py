#!/usr/bin/env python3
"""Issue #3420: factory refuse-at-required (residual of #3326).

#3326 shipped create_with_cover + first-wave sites. Production required
still inventoried both-null default create. #3420 refuses the allocate
(nullptr) so no live uncovered object survives the call.

Contract:
  AC1 allocate_raw_impl refuses required + both-null + not render
  AC2 Soft default create stays legal; uncovered does not grow on covered
  AC3 remaining src/ hot .create< migrated or listed residual
  AC4 soak: cover-compliant uncovered stable
  AC5 Soft: required_active load, no extra pin atomics
  AC6 no second registry / no new query keys / no g_3420_*
  AC7 tests in test_moving_densify_fail_closed +
      test_arena_required_cover_no_value_only; linter after #3326;
      no docs/design/3420-*; no test_issue_3420.cpp

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

    arena = _read("src/core/arena.ixx")
    lp = _read("src/core/lifetime_pin.hh")
    fail_closed = _read("tests/core/test_moving_densify_fail_closed.cpp")
    cover = _read("tests/core/test_arena_required_cover_no_value_only.cpp")
    build = _read("build.py")
    svc = _read("src/compiler/service.ixx")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    zc = _read("src/core/zero_copy_output.hh")

    must("kFactoryRefuseUncoveredIssue = 3420", "AC1 arena stamp", arena)
    must("kFactoryRefuseUncoveredIssue = 3420", "AC1 pin stamp", lp)
    must("Issue #3420", "AC1 arena cite", arena)
    must("factory_uncovered_refused_", "AC1 refuse helper", arena)
    helper = arena.find("factory_uncovered_refused_")
    helper_win = arena[helper : helper + 900] if helper >= 0 else ""
    must("general_object_pin_required_active()", "AC1 required load", helper_win)
    must("cover_slot != nullptr || cover_reason != nullptr", "AC1 both-null", helper_win)
    must(
        "g_intermediate_create_uncovered_under_required_total.fetch_add",
        "AC1 reuse uncovered metric",
        helper_win,
    )
    impl = arena.find("void* allocate_raw_impl(std::size_t size, std::size_t alignment")
    impl_win = arena[impl : impl + 800] if impl >= 0 else ""
    must("factory_uncovered_refused_(cover_slot, cover_reason)", "AC1 impl calls helper", impl_win)
    must("return nullptr", "AC1 refuse", impl_win)
    if "intermediate_creates_.push_back" in impl_win:
        fails.append("AC1: refuse path must not inventory in allocate_raw_impl")

    must("ac3420_1_factory_refuses_both_null", "AC1 test", fail_closed)
    must("ac3326_3_try_allocate_cover_and_soft", "AC2 Soft create", fail_closed)
    must("ac3420_3_cover_compliant_soak", "AC4 soak", fail_closed)

    if ".create<" in svc:
        fails.append("AC3: src/compiler/service.ixx still has default .create<")
    if ".create<" in mut:
        fails.append("AC3: evaluator_primitives_mutate.cpp still has default .create<")
    must("Issue #3420 residual", "AC3 zero_copy residual", zc)

    must("general_object_pin_required_active()", "AC5 required load", helper_win)
    if "LifetimePin::pin" in helper_win or ".pin(" in helper_win:
        fails.append("AC5: extra pin atomic on factory refuse path")

    if "g_moving_pin_registry_3420" in arena or "class DensifyPinRegistry" in arena:
        fails.append("AC6: second pin registry introduced")
    if "g_3420_" in arena or "g_3420_" in lp:
        fails.append("AC6: invented g_3420_* counter")
    if "schema-3420" in arena or "schema-3420" in lp:
        fails.append("AC6: new schema-3420 query key")

    must("check_factory_refuse_uncovered_3420", "AC7 build.py", build)
    must("ac3420_factory_refuse_surface", "AC7 required-cover suite", cover)
    prev = build.find("check_factory_default_cover_3326")
    ours = build.find("check_factory_refuse_uncovered_3420")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC7: #3420 linter must run after #3326")
    if (ROOT / "tests" / "issues" / "test_issue_3420.cpp").is_file():
        fails.append("AC7: forbidden tests/issues/test_issue_3420.cpp per #81967")
    if (ROOT / "tests" / "core" / "test_issue_3420.cpp").is_file():
        fails.append("AC7: forbidden tests/core/test_issue_3420.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3420-*")):
            fails.append(f"AC7: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3420 factory_refuse_uncovered:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3420 factory_refuse_uncovered: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
