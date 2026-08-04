#!/usr/bin/env python3
"""Issue #2524: giant module split — partition map + pass_manager Phase C.

Contract:
  AC1 measurable facade shrink + partition map
  AC2 facade re-exports; no public API renames
  AC3 hot-path smoke / types usable
  AC4 dependency graph documented + CMake order
  AC5 no circular imports (core ← impls ← facade)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _size(rel: str) -> int:
    p = ROOT / rel
    return p.stat().st_size if p.is_file() else 0


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    facade = _read("src/compiler/pass_manager.ixx")
    core = _read("src/compiler/pass_pipeline_core.ixx")
    impls = _read("src/compiler/pass_impls.ixx")
    cmake = _read("cmake/AuraModules.cmake")
    test = _read("tests/compiler/test_module_partition_map_2524.cpp")
    build = _read("build.py")
    clists = _read("CMakeLists.txt")

    # AC1 — partition map lives as facade header (no docs/design/ plan docs)
    must("Issue #2524", "AC1", facade)
    must("export import aura.compiler.pass_pipeline_core", "AC1", facade)
    must("export import aura.compiler.pass_impls", "AC1", facade)
    must("export module aura.compiler.pass_pipeline_core", "AC1", core)
    must("export module aura.compiler.pass_impls", "AC1", impls)
    must("Module partition map", "AC1", facade)
    must("Phase A", "AC1", facade)
    must("Phase B", "AC1", facade)
    must("Phase C", "AC1", facade)
    must("ac1_size_and_map", "AC1", test)
    if _size("src/compiler/pass_manager.ixx") >= 20_000:
        fails.append(f"AC1: pass_manager facade too large ({_size('src/compiler/pass_manager.ixx')} bytes)")
    if _size("src/compiler/pass_pipeline_core.ixx") >= 150_000:
        fails.append("AC1: pass_pipeline_core exceeds 150 KB")
    if _size("src/compiler/pass_impls.ixx") < 100_000:
        fails.append("AC1: pass_impls missing extracted bodies")

    # AC2
    must("ComputeKindWrap", "AC2", impls)
    must("InlinePass", "AC2", impls)
    must("DeadCoercionEliminationPass", "AC2", impls)
    must("DefineDirtyMaskView", "AC2", core)
    must("run_pipeline", "AC2", core)
    must("ac2_public_api", "AC2", test)

    # AC3
    must("ac3_hot_path_smoke", "AC3", test)

    # AC4
    must("Facade only re-exports", "AC4", facade)
    must("pass_pipeline_core.ixx", "AC4", cmake)
    must("pass_impls.ixx", "AC4", cmake)
    p_core = cmake.find("pass_pipeline_core.ixx")
    p_impl = cmake.find("pass_impls.ixx")
    p_facade = cmake.find("pass_manager.ixx")
    if not (0 <= p_core < p_impl < p_facade):
        fails.append("AC4: CMake order must be core < impls < facade")
    must("ac4_dep_graph_doc", "AC4", test)

    # AC5 — real import edges only (ignore comment mentions of sibling modules)
    if "\nimport aura.compiler.pass_impls" in core or "\nexport import aura.compiler.pass_impls" in core:
        fails.append("AC5: core imports pass_impls (cycle risk)")
    if "\nimport aura.compiler.pass_manager" in impls or "\nexport import aura.compiler.pass_manager" in impls:
        fails.append("AC5: impls imports facade (cycle)")
    must("export import aura.compiler.pass_pipeline_core", "AC5", impls)
    must("ac5_no_cycles", "AC5", test)

    # Gate
    must("test_module_partition_map_2524", "gate", clists)
    must("check_module_partition_map_2524", "gate", build)
    must("cmd_module_partition_map_coverage", "gate", build)

    # Retain evaluator/ast size awareness in map
    must("evaluator.ixx", "retain", facade)
    must("ast.ixx", "retain", facade)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2524 module partition map + pass_manager Phase C — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
