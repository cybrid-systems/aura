#!/usr/bin/env python3
"""Issue #3833: IRFunctionSoA columns Arena-backed (BMI offsetof pins kept).

IR SoA opcode/operand/metadata columns were std::vector ("defer arena
migration to Phase 3"). FlatAST closed Arena locality; IR SoA still hit
default heap. Fix: IrSoaArenaColumn (24-byte, same sizeof as std::vector)
backed by per-function monotonic slab — keeps #3314 BMI pins.

Contract:
  AC1 No std::vector instruction columns; IrSoaArenaColumn + bind + slab
  AC2 view_at / batch dirty / HARDEN / #3314 pins unchanged
  AC3 Microbench in test_ir_soa_layout_stamp; build wiring; no invent

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

    def forbid(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    soa = _read("src/compiler/ir_soa.ixx")
    test = _read("tests/compiler/test_ir_soa_layout_stamp.cpp")
    build = _read("build.py")

    must("kIrSoaColumnArenaIssue = 3833", "AC1 stamp", soa)
    must("Issue #3833", "AC1 cite", soa)
    must("IrSoaArenaColumn", "AC1 column type", soa)
    must("IrSoaArenaColumn<aura::ir::IROpcode> opcodes_", "AC1 opcodes_", soa)
    must("bind_column_arena", "AC1 bind", soa)
    must("column_slab_for", "AC1 monotonic slab", soa)
    must("sizeof(IrSoaArenaColumn<std::uint32_t>) == sizeof(std::vector<std::uint32_t>)", "AC1 BMI size match", soa)
    forbid("std::vector<aura::ir::IROpcode> opcodes_", "AC1 no vector opcodes_", soa)
    forbid("defer arena migration to Phase 3", "AC1 Phase 3 deferral removed", soa)

    must("offsetof(IRFunctionSoA, block_dirty_) == 376", "AC2 BMI block_dirty_", soa)
    must("offsetof(IRFunctionSoA, instruction_dirty_) == 408", "AC2 BMI instruction_dirty_", soa)
    must("offsetof(IRFunctionSoA, generation_) == 440", "AC2 BMI generation_", soa)
    must("sizeof(IRFunctionSoA) == 448", "AC2 BMI sizeof", soa)
    must("view_at", "AC2 view_at retained", soa)
    must("mark_blocks_dirty", "AC2 batch dirty retained", soa)

    must("3833 AC1", "AC3 test AC1", test)
    must("3833 AC2", "AC3 test AC2", test)
    must("3833 AC3", "AC3 microbench", test)
    must("walk_soa_function_hotpath", "AC3 walk", test)
    must("check_ir_soa_column_arena_3833", "AC3 build.py", build)

    if (ROOT / "tests" / "compiler" / "test_issue_3833.cpp").is_file():
        fails.append("AC3: forbidden tests/compiler/test_issue_3833.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3833.cpp").is_file():
        fails.append("AC3: forbidden tests/issues/test_issue_3833.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3833-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden)")

    if fails:
        print("FAIL #3833 ir_soa_column_arena:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3833 ir_soa_column_arena: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
