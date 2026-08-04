#!/usr/bin/env python3
"""Issue #2615: production multi-block dirty cascades use batch mark_blocks_dirty.

Contract:
  AC1 DCE / impact_scope use mark_blocks_dirty / mark_blocks_dirty_bit_only
  AC2 mark_block_dirty retained for single-block (single marks counter)
  AC3 mark_blocks_dirty_bits_only + finish_dirty_sync path
  AC4 residual multi mark_block_dirty loops banned in hot production files
  AC5 schema-2615 fence metrics + test/cmake/build gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Production files that must not contain multi-block mark_block_dirty loops.
HOT_FILES = [
    "src/compiler/pass_impls.ixx",
    "src/compiler/service.ixx",
    "src/compiler/service_dirty.cpp",
    "src/compiler/dirty_propagation.ixx",
]


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

    soa = _read("src/compiler/ir_soa.ixx")
    svc = _read("src/compiler/service.ixx")
    dce = _read("src/compiler/pass_impls.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_batch_dirty_discipline_2615.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2615", "AC1", soa)
    must("mark_blocks_dirty_bits_only", "AC1", soa)
    must("g_ir_soa_batch_dirty_cascades_total", "AC1", soa)
    must("kIrSoaBatchDirtyDisciplineIssue", "AC1", soa)
    must("Issue #2615", "AC1", dce)
    must("mark_blocks_dirty(changed_blocks)", "AC1", dce)
    must("mark_blocks_dirty_bit_only", "AC1", svc)
    must("ac1_multi_batch", "AC1", test)

    # AC2
    must("g_ir_soa_single_dirty_marks_total", "AC2", soa)
    must("ac2_single_unchanged", "AC2", test)

    # AC3
    must("mark_block_dirty_bit_only_no_bump", "AC3", soa)
    must("ac3_finish_sync", "AC3", test)

    # AC4 residual loop ban (heuristic: for + mark_block_dirty in same short window)
    residual_pat = re.compile(
        r"for\s*\([^;{]{0,120}\)[^{]{0,80}\{[^}]{0,400}mark_block_dirty\s*\(",
        re.MULTILINE | re.DOTALL,
    )
    # Allow mark_blocks_dirty / mark_block_dirty_no_bump / bit_only batch
    for rel in HOT_FILES:
        hay = _read(rel)
        if not hay:
            continue
        # Strip comments roughly
        stripped = re.sub(r"//[^\n]*", "", hay)
        for m in residual_pat.finditer(stripped):
            snippet = m.group(0)
            # Skip if the loop body only uses mark_blocks_dirty / bit_only batch
            if "mark_blocks_dirty" in snippet and "mark_block_dirty(" not in snippet.replace(
                "mark_blocks_dirty", ""
            ).replace("mark_block_dirty_bit_only", "").replace("mark_block_dirty_no_bump", "").replace(
                "mark_block_dirty_impl", ""
            ):
                continue
            # Flag if bare mark_block_dirty( appears in for-loop body over multi
            if (
                re.search(r"mark_block_dirty\s*\(", snippet)
                and not re.search(r"mark_block_dirty_(bit_only_no_bump|no_bump|impl|bits_only)", snippet)
                and "mark_blocks_dirty" not in snippet
            ):
                # mark_block_dirty( alone — residual multi-block loop
                fails.append(f"AC4: residual multi mark_block_dirty loop in {rel}: {snippet[:120]!r}...")

    # Explicit: DCE run body must not mark_block_dirty(block.block_id)
    if "mark_block_dirty(block.block_id)" in dce:
        fails.append("AC4: pass_impls still has mark_block_dirty(block.block_id)")

    must("ac4_no_residual_loops", "AC4", test)

    # AC5
    must("schema-2615", "AC5", q)
    must("soa-batch-dirty-cascades-total", "AC5", q)
    must("soa-dirty-fence-total", "AC5", q)
    must("test_batch_dirty_discipline_2615", "AC5", cmake)
    must("check_batch_dirty_discipline_2615", "AC5", build)
    must("cmd_batch_dirty_discipline_coverage", "AC5", build)
    must("ac5_fence_rate", "AC5", test)

    for rel in (
        "docs/design/batch_dirty_discipline_2615.md",
        "docs/batch_dirty_discipline_2615.md",
        "design/2615.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2615 batch dirty cascade discipline — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
