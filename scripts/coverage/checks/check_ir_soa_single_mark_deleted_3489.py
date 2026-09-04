#!/usr/bin/env python3
"""Issue #3489: production pack deletes IRFunctionSoA::mark_block_dirty.

#3355 banned production *call sites* via path grep. Residual: the symbol
still compiled a resize + single-fence body. A TU that evades the linter
still heap-grows per id. Batch helpers resized inside the span loop.

Contract:
  AC1  production pack (`AURA_PRODUCTION_PACK` / AURA_IR_SOA_ONLY+NDEBUG)
       `= delete`s mark_block_dirty; compile-fail fixture exists
  AC2  batch ensure once before the span walk; no per-id resize
  AC3  Soft/unit keep mark_block_dirty; residual abort path stays
  AC4  test_batch_dirty_discipline + #3355 linter kept; this linter AFTER
  AC5  empty span quiet; no extra atomics / no new g_3489_* / schema-3489

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

    soa = _read("src/compiler/ir_soa.ixx")
    cmake = _read("CMakeLists.txt")
    atest = _read("cmake/AuraTest.cmake")
    t = _read("tests/compiler/test_batch_dirty_discipline.cpp")
    fixture = _read("tests/compiler/compile_fail_mark_block_dirty_3489.cpp")
    l3355 = _read("scripts/coverage/checks/check_ir_soa_single_mark_production_ban_3355.py")
    build = _read("build.py")

    must("kIrSoaSingleMarkDeletedIssue = 3489", "AC1 stamp", soa)
    must("void mark_block_dirty(std::uint32_t block_id) = delete", "AC1 = delete", soa)
    must("AURA_PRODUCTION_PACK", "AC1 production pack ifdef", soa)
    must("AURA_IR_SOA_SINGLE_MARK_DELETED", "AC1 delete macro", soa)
    must("AURA_PRODUCTION_PACK=1", "AC1 aura target", cmake)
    must("AURA_ALLOW_IR_SOA_SINGLE_MARK=1", "AC3 test allow", atest)
    must("mark_block_dirty(0)", "AC1 compile-fail fixture call", fixture)
    must("AURA_PRODUCTION_PACK", "AC1 fixture gated", fixture)

    must("ensure_block_dirty_len", "AC2 ensure block", soa)
    must("ensure_instruction_dirty_len", "AC2 ensure instr", soa)
    must("mark_block_dirty_bit_or_cascade_no_resize", "AC2 no-resize cascade", soa)
    must("mark_block_dirty_bit_only_no_resize", "AC2 no-resize bits", soa)

    batch = soa.find("inline void IRFunctionSoA::mark_blocks_dirty(")
    bits = soa.find("inline void IRFunctionSoA::mark_blocks_dirty_bits_only(")
    allb = soa.find("inline void IRFunctionSoA::mark_all_blocks_dirty()")
    bwin = soa[batch:bits] if batch >= 0 and bits > batch else ""
    bitwin = soa[bits:allb] if bits >= 0 and allb > bits else ""
    must("ensure_block_dirty_len", "AC2 batch ensure", bwin)
    must("mark_block_dirty_bit_or_cascade_no_resize", "AC2 batch no-resize walk", bwin)
    if ".resize(" in bwin:
        fails.append("AC2: mark_blocks_dirty still resize()s in the batch body")
    must("ensure_block_dirty_len", "AC2 bits-only ensure", bitwin)
    must("mark_block_dirty_bit_only_no_resize", "AC2 bits-only no-resize walk", bitwin)
    if ".resize(" in bitwin:
        fails.append("AC2: mark_blocks_dirty_bits_only still resize()s in the batch body")

    must("ac3489_3_soft_single_remains", "AC3 Soft test", t)
    must("std::abort()", "AC3 abort path", soa)
    must("note_single_mark_for_residual", "AC3 residual helper", soa)

    must("ac3489_1_production_symbol_deleted", "AC4 AC1 test", t)
    must("ac3489_2_batch_one_ensure", "AC4 AC2 test", t)
    must("SINGLE_MARK_RE", "AC4 #3355 grep kept", l3355)
    must("check_ir_soa_single_mark_deleted_3489", "AC4 build.py", build)
    prev = build.find("check_ir_soa_single_mark_production_ban_3355")
    ours = build.find("check_ir_soa_single_mark_deleted_3489")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3355")

    empty = bwin.find("if (block_ids.empty())")
    eret = bwin.find("return", empty) if empty >= 0 else -1
    bump = bwin.find("bump_generation()")
    if empty < 0 or eret < 0 or bump < 0 or not (empty < eret < bump):
        fails.append("AC5: empty span must return before bump_generation")
    atomics_before_loop = bwin[: bwin.find("for (const auto block_id")] if "for (const auto block_id" in bwin else bwin
    if "fetch_add" in atomics_before_loop:
        fails.append("AC5: extra atomics on batch happy/empty path")

    must_not("schema-3489", "AC5 no schema-3489", soa)
    must_not("g_3489_", "AC5 no g_3489_*", soa)
    if (ROOT / "tests" / "compiler" / "test_issue_3489.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3489.cpp present")
    if (ROOT / "tests" / "issues" / "test_issue_3489.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3489.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3489-*")):
            fails.append(f"AC4: docs/design/{f.name} present")

    if fails:
        print("FAIL #3489 ir_soa_single_mark_deleted:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3489 ir_soa_single_mark_deleted: production = delete; batch one ensure")
    return 0


if __name__ == "__main__":
    sys.exit(main())
