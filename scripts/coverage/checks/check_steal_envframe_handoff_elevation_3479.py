#!/usr/bin/env python3
"""Issue #3479: FiberSteal EnvFrame cell/closure elevation (dummy handoff_ref gone).

#2632 AC3 stub called handoff_ref on a dummy StableNodeRef. Closure/cell
arena pointers in refreshed frames were not slotted or canaried.
Production now walks EvalValue variants: lasting member ptrs skipped
(#3368 XOR); else #3274 slot XOR #3210 canary; real body_id handoff_ref.
Soft / empty bindings: no extra walk.

Contract:
  AC1  dummy StableNodeRef gone; walk is_closure/is_cell; elevate
  AC2  production_defaults_active gate; empty bindings skip
  AC3  #3368 XOR lasting member ptr
  AC4  no schema-3479 / g_3479_*
  AC5  extend test_fiber_migration_refresh; linter AFTER #2632

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
            fails.append(f"{label}: forbidden {n!r}")

    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/serve/test_fiber_migration_refresh.cpp")
    build = _read("build.py")

    start = fm.find("std::size_t Evaluator::refresh_stale_frames_after_steal")
    if start < 0:
        fails.append("AC1: refresh_stale_frames_after_steal missing")
        win = ""
    else:
        win = fm[start : start + 9000]
        must("Issue #3479", "AC1 cite", win)
        must("is_closure", "AC1 closure walk", win)
        must("is_cell", "AC1 cell walk", win)
        must("note_ffi_opaque_alias_densify_cover", "AC1 elevate", win)
        must("handoff_ref(make_stamped_ref", "AC1 real handoff", win)
        must_not("StableNodeRef dummy", "AC1 no dummy", win)
        must_not("handoff_ref(std::move(dummy))", "AC1 no dummy call", win)
        must("production_defaults_active()", "AC2 production gate", win)
        must("bindings_symid_.empty()", "AC2 empty skip", win)
        must("lasting_member_ptr", "AC3 XOR helper", win)
        must("do not dual-note", "AC3 XOR cite", win)

    must("handoff_ref(", "2632 AC3 still present", fm)
    must("2632 AC3", "2632 AC3 comment", fm)

    must("3479 AC1: dummy StableNodeRef gone", "AC5 AC1", test)
    must("3479 AC5: apply after steal refresh uses live identity or refuses cleanly", "AC5 live", test)
    must("3479 AC2: Soft refresh completes", "AC5 AC2", test)
    must("3479 AC3: XOR skip lasting member ptr", "AC5 AC3", test)

    must_not("schema-3479", "AC4 no query key", fm)
    must_not("g_3479_", "AC4 no g_3479_*", fm)

    must("check_steal_envframe_handoff_elevation_3479", "AC5 build.py", build)
    prev = build.find("check_export_held_handoff_coverage")
    ours = build.find("check_steal_envframe_handoff_elevation_3479")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #2632")

    if (ROOT / "tests" / "compiler" / "test_issue_3479.cpp").is_file():
        fails.append("AC5: test_issue_3479.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3479-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3479 steal_envframe_handoff_elevation:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3479 steal_envframe_handoff_elevation: dummy gone; cell/closure elevated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
