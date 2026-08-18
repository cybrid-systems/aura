#!/usr/bin/env python3
"""Issue #3124: non-allocating arena compact/layout/root_remap hooks.

Replace std::function hook slots with {fn, ctx} function pointers.
Compact uses a fixed 4-slot table so Evaluator re_pin + CompilerService
Shape inval install independently. Soft/sandbox unchanged.

Contract:
  AC1 APIs no longer take std::function; no heap on set/invoke
  AC2 Production listeners still fire (slots, not take+chain)
  AC3 Concurrent set/take/invoke stays mutex-serialized
  AC4 Dtor still nulls slots before run_destructors
  AC5 Extend existing hook tests; no test_issue_3124.cpp

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

    def must_absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    arena = _read("src/core/arena.ixx")
    ev = _read("src/compiler/evaluator.ixx")
    svc = _read("src/compiler/service.ixx")
    rrp = _read("src/compiler/root_remap_pass.ixx")
    t = _read("tests/core/test_has_on_compact_hook_lock.cpp")
    build = _read("build.py")

    must("Issue #3124", "AC1 arena", arena)
    must("CompactHookFn", "AC1 CompactHookFn", arena)
    must("LayoutChangeHookFn", "AC1 layout fn", arena)
    must("RootRemapHookFn", "AC1 root fn", arena)
    must("kArenaCompactHookSlots = 4", "AC1 slot count", arena)
    must("set_on_compact_hook(CompactHookFn fn", "AC1 compact set", arena)
    must("set_on_layout_change(LayoutChangeHookFn fn", "AC1 layout set", arena)
    must("set_root_remap_callback(RootRemapHookFn fn", "AC1 root set", arena)
    must_absent("set_on_compact_hook(std::function", "AC1 no std::function compact", arena)
    must("ac3124_1_no_std_function_api", "AC1 test", t)

    must("on_arena_compact_hook_thunk", "AC2 evaluator thunk", ev)
    must("on_arena_compact_hook_thunk", "AC2 service thunk", svc)
    must("root_remap_pass_hook_fn", "AC2 root remap fn", rrp)
    must("ac3124_2_two_slots_fire", "AC2 test", t)

    must("hook_mtx_", "AC3 compact mutex", arena)
    must("on_layout_change_mtx_", "AC3 layout mutex", arena)
    must("root_remap_mtx_", "AC3 root mutex", arena)

    must("slot.fn = nullptr", "AC4 dtor compact", arena)
    must("on_layout_change_.fn = nullptr", "AC4 dtor layout", arena)
    must("root_remap_.fn = nullptr", "AC4 dtor root", arena)
    must("run_destructors();", "AC4 run_destructors", arena)

    must("kNonAllocatingArenaHooksIssue = 3124", "AC5 stamp", arena)
    must("ac3124_5_source_and_linter", "AC5 test", t)
    must("check_nonalloc_arena_hooks_3124", "AC5 build.py", build)
    if (ROOT / "tests" / "core" / "test_issue_3124.cpp").is_file():
        fails.append("AC5: test_issue_3124.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_3124.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3124.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3124-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3124 non-allocating arena hooks — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
