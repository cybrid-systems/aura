#!/usr/bin/env python3
"""Issue #2522: batch dirty cascade API — mark_blocks_dirty + single bump.

Contract:
  AC1 batch API; one generation bump regardless of block count
  AC2 semantics match sequential mark_block_dirty
  AC3 mark_all_blocks_dirty bulk single bump
  AC4 fewer fence advances for N-block batch vs N× single
  AC5 finish_dirty_sync holds instruction_dirty_synced_with_blocks

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

    hh = _read("src/compiler/ir_soa.ixx")
    svc = _read("src/compiler/service.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_batch_dirty_cascade_2522.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2522", "AC1", hh)
    must("mark_blocks_dirty", "AC1", hh)
    must("mark_instruction_range_dirty", "AC1", hh)
    must("kIrSoaBatchDirtyIssue", "AC1", hh)
    must("bump_generation(); // once regardless of block count", "AC1", hh)
    must("ac1_batch_one_bump", "AC1", test)

    # AC2
    must("mark_block_dirty_no_bump", "AC2", hh)
    must("fill_instruction_dirty_range", "AC2", hh)
    must("ac2_semantics_match", "AC2", test)

    # AC3
    must("std::fill(block_dirty_", "AC3", hh)
    must("std::fill(instruction_dirty_", "AC3", hh)
    must("ac3_full_function_single_bump", "AC3", test)

    # AC4
    must("g_ir_soa_generation_fence", "AC4", hh)
    must("ac4_fence_fewer_advances", "AC4", test)

    # AC5
    must("finish_dirty_sync", "AC5", hh)
    must("instruction_dirty_synced_with_blocks", "AC5", hh)
    must("mark_function_blocks_dirty", "AC5", hh)
    must("schema-2522", "AC5", q)
    must("soa-batch-dirty-wired", "AC5", q)
    must("#2522", "AC5", svc)
    must("mark_blocks_dirty", "AC5", svc)
    must("force_soa_instruction_dirty_sync", "AC5", svc)
    must("test_batch_dirty_cascade_2522", "AC5", cmake)
    must("check_batch_dirty_cascade_2522", "AC5", build)
    must("cmd_batch_dirty_cascade_coverage", "AC5", build)
    must("ac5_finish_dirty_sync", "AC5", test)

    # Retain lineage
    must("schema-2139", "retain", q)
    must("finish_dirty_sync", "retain", svc)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2522 batch dirty cascade — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
