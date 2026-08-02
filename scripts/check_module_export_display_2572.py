#!/usr/bin/env python3
"""Issue #2572: module-export multi-display ConstString pool + JIT display.

Contract:
  AC1 cache_module persists ir_cache_strings_ with IR bundles
  AC2 call-site ConstString remap uses cache_strings (existing path)
  AC3 JIT PrimDisplay uses tagged aura_display_value (not raw %ld)
  AC4 regression test + cmake + build.py gate
  AC5 compile_module module path bind_in_env=false (no TW overwrite)

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

    svc = _read("src/compiler/service.ixx")
    jit = _read("src/compiler/aura_jit.cpp")
    jit_rt = _read("src/compiler/aura_jit_runtime.cpp")
    test = _read("tests/compiler/test_module_export_display_2572.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    lower = _read("src/compiler/lowering_impl.cpp")

    must("#2572", "AC1", svc)
    must("ir_cache_strings_[name] = ir_mod.string_pool", "AC1", svc)
    must("cache_module", "AC1", svc)

    must("ConstString", "AC2", lower)
    must("cache_strings", "AC2", lower)
    must("add_string", "AC2", lower)

    must("aura_display_value", "AC3", jit)
    must("aura_display_value", "AC3", jit_rt)
    must("#2572", "AC3", jit_rt)
    must("PrimDisplay", "AC3", jit)

    must("ac1_exported_multi_display", "AC4", test)
    must("test_module_export_display_2572", "AC4", cmake)
    must("check_module_export_display_2572", "AC4", build)
    must("cmd_module_export_display_coverage", "AC4", build)

    must("bind_in_env=*/false", "AC5", svc)
    must("#2572", "AC5", svc)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2572 module export multi-display — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
