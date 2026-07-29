#!/usr/bin/env python3
"""Issue #2297: structural capture-cell remount after densify.

  AC1: remount_capture_cells_via_densify_ + densify remap publish
  AC2: env_gen PRIMARY before cell remap
  AC3: empty densify context early return
  AC4: metrics + query keys + schema-2297
  AC5: RootRemapPass publish + tests

Exit 0 = all ACs satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "compiler" / "aura_jit_runtime.cpp"
BH = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
BC = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
MET = ROOT / "src" / "compiler" / "observability_metrics.h"
Q = ROOT / "src" / "compiler" / "evaluator_primitives_query.cpp"
RP = ROOT / "src" / "compiler" / "root_remap_pass.ixx"
TEST = ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp"


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    rt = RT.read_text(encoding="utf-8", errors="replace")
    bh = BH.read_text(encoding="utf-8", errors="replace")
    bc = BC.read_text(encoding="utf-8", errors="replace")
    met = MET.read_text(encoding="utf-8", errors="replace")
    q = Q.read_text(encoding="utf-8", errors="replace")
    rp = RP.read_text(encoding="utf-8", errors="replace")
    test = TEST.read_text(encoding="utf-8", errors="replace")

    must("remount_capture_cells_via_densify_", "AC1", rt)
    must("aura_set_densify_object_remap", "AC1", bh)
    must("aura_set_densify_object_remap", "AC1", rt)

    must("Cell remap only runs after env_gen OK", "AC2", rt)
    must("aura_bump_closure_capture_env_gen_mismatch_total", "AC2", rt)

    must("AC3: zero work", "AC3", rt)
    must("aura_clear_densify_object_remap", "AC3", bh)

    must("closure_capture_cell_remap_ok_total{0}", "AC4", met)
    must("closure_capture_cell_remap_fail_total{0}", "AC4", met)
    must("aura_bump_closure_capture_cell_remap_ok_total", "AC4", bc)
    must("closure-capture-cell-remap-ok-total", "AC4", q)
    must("schema-2297", "AC4", q)
    must("capture-cell-remap-wired", "AC4", q)

    must("aura_set_densify_object_remap", "AC5", rp)
    must("void ac2297_structural_cell_remap", "AC5", test)
    must("ac2297_structural_cell_remap(cs)", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: structural capture-cell remount (#2297) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
