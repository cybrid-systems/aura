#!/usr/bin/env python3
"""Issue #3326: factory-default create<T> / try_allocate cover at allocate site.

Residual of #3180/#3214/#3156: public create<T> and try_allocate still note
uncovered (both-null) under production required, so densify-tracked
intermediates sticky-off even when the caller later declares cover.

#3326 adds create_with_cover + try_allocate cover args so cover-compliant
sites skip the uncovered bump. Default create stays both-null (Soft/compat)
and still fail-closes Moving. No second pin registry; no new query keys.

Contract:
  AC1 Factory surface: create_with_cover(slot, reason, args) + try_allocate
      cover args thread through allocate_raw.
  AC2 Default create without cover still fail-closes (existing gate).
  AC3 Slot cover at allocate: uncovered does not grow; sticky not armed
      solely by that create.
  AC4 Soft: maybe_note still single required-active load; default create
      does not inventory.
  AC5 No second registry / no new query keys / no g_3326_*.
  AC6 Tests in test_moving_densify_fail_closed +
      test_arena_required_cover_no_value_only; linter in build.py;
      no docs/design/3326-*; no test_issue_3326.cpp.

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
    eval_flat = _read("src/compiler/evaluator_eval_flat.cpp")

    must("kFactoryDefaultCoverIssue = 3326", "AC1 arena stamp", arena)
    must("kFactoryDefaultCoverIssue = 3326", "AC1 pin stamp", lp)
    must("Issue #3326", "AC1 arena cite", arena)
    must("create_with_cover", "AC1 create_with_cover", arena)
    must("try_allocate(std::size_t size, void** cover_slot", "AC1 try_allocate cover", arena)
    must(
        "allocate_raw(sizeof(T), alignof(T), cover_slot, cover_reason)",
        "AC1 create_with_cover forwards cover",
        arena,
    )
    must("register_external_root_slot_for_densify(cover_slot)", "AC1 slot rewrite after construct", arena)

    must("ac3326_1_create_without_cover_fail_closed", "AC2 test", fail_closed)
    must("ac3326_2_create_with_cover_no_uncovered_sticky", "AC3 test", fail_closed)
    must("ac3326_3_try_allocate_cover_and_soft", "AC4 test", fail_closed)
    must("ac3326_factory_cover_surface", "AC6 required-cover suite", cover)

    if "general_object_pin_required_active" not in arena:
        fails.append("AC4: required_active load missing")

    if "g_moving_pin_registry_3326" in arena or "class DensifyPinRegistry" in arena:
        fails.append("AC5: second pin registry introduced")
    if "g_3326_" in arena:
        fails.append("AC5: invented g_3326_* counter")
    must("check_factory_default_cover_3326", "AC6 build.py", build)
    must("create_with_cover", "AC6 hot-path eval_flat migrated", eval_flat)

    if (ROOT / "tests" / "issues" / "test_issue_3326.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3326.cpp per #81967")
    if (ROOT / "tests" / "core" / "test_issue_3326.cpp").is_file():
        fails.append("AC6: forbidden tests/core/test_issue_3326.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3326-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3326 factory_default_cover:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3326 factory_default_cover: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
