#!/usr/bin/env python3
"""Issue #2382: ASTArena dtor clears hooks before internal teardown.

Contract:
  AC1 Dtor nulls on_compact_hook_ / on_layout_change_ / root_remap_ under mtx
  AC2 Hook clear precedes run_destructors(); test proves capture destroyed
  AC3 Concurrent invoke + destroy stress test surface
  AC4 CMake + build.py gate + source-cite

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    arena = _read("src/core/arena.ixx")
    test = _read("tests/core/test_arena_dtor_clears_hooks_2382.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 dtor clears under locks
    must("Issue #2382", "AC1", arena)
    must("~ASTArena()", "AC1", arena)
    must("on_compact_hook_ = nullptr", "AC1", arena)
    must("on_layout_change_ = nullptr", "AC1", arena)
    must("root_remap_ = nullptr", "AC1", arena)
    must("hook_mtx_", "AC1", arena)
    must("on_layout_change_mtx_", "AC1", arena)
    must("root_remap_mtx_", "AC1", arena)

    # AC2 order + test — clear all three hooks then call run_destructors in ~ASTArena.
    # Prefer the real dtor definition (not a comment mentioning ~ASTArena()).
    dtor_i = arena.find("~ASTArena() {")
    if dtor_i < 0:
        fails.append("AC2: ~ASTArena() { not found")
    else:
        dtor_snip = arena[dtor_i : dtor_i + 2500]
        clear_i = dtor_snip.find("on_compact_hook_ = nullptr")
        layout_i = dtor_snip.find("on_layout_change_ = nullptr")
        root_i = dtor_snip.find("root_remap_ = nullptr")
        run_i = dtor_snip.find("run_destructors();")
        if clear_i < 0 or layout_i < 0 or root_i < 0 or run_i < 0:
            fails.append("AC2: dtor must null all three hooks and call run_destructors()")
        elif not (clear_i < run_i and layout_i < run_i and root_i < run_i):
            fails.append("AC2: hook clears must precede run_destructors() inside ~ASTArena")
    must("ac1_ac2_dtor_clears_all_hooks", "AC2", test)
    must("use_count() == 1", "AC2", test)
    must("has_on_compact_hook", "AC2", test)
    must("has_on_layout_change", "AC2", test)
    must("has_root_remap_callback", "AC2", test)

    # AC3 race stress
    must("ac3_concurrent_invoke_and_destroy", "AC3", test)
    must("invoke_on_compact_hook_for_test", "AC3", test)

    # AC4 registration
    must("test_arena_dtor_clears_hooks_2382", "AC4", cmake)
    must("check_arena_dtor_clears_hooks_2382", "AC4", build)
    must("cmd_arena_dtor_clears_hooks_coverage", "AC4", build)
    must("ac4_source_and_registration", "AC4", test)
    must("Issue #2382", "AC4", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2382 ASTArena dtor clears hooks — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
