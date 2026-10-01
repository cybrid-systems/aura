#!/usr/bin/env python3
"""Issue #4263: IrSoaColumnSlab upstream Arena-owned past 8KiB seed.

#3853 left optional: each IrSoaColumnSlab seeds 8KiB then upstreams to
std::pmr::new_delete_resource(). Large-fn / sustained dual-lower×dirty
past seed hit implicit heap. Fix: slab-owned IrSoaSlabUpstream retains
growth chunks until slab drop — not default new_delete.

Contract (one row per AC):
  AC1  IrSoaColumnSlab resource upstreams to IrSoaSlabUpstream (not new_delete)
  AC2  #3833/#3853 BMI offsetof / sizeof pins still hold; bind path intact
  AC3  extends test_ir_soa_layout_stamp; linter + grandfather + manifest
       + build.py; no invent / docs/design; no g_4263_* counters

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOA = "src/compiler/ir_soa.ixx"
TEST = "tests/compiler/test_ir_soa_layout_stamp.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/4263.json"
LINTER = "check_ir_soa_column_slab_upstream_4263"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _strip_comments(src: str) -> str:
    return re.sub(r"//[^\n]*", "", src)


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    soa = _read(SOA)
    soa_code = _strip_comments(soa)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    # ── AC1: Arena-owned upstream ──
    must("Issue #4263", "AC1 cite", soa)
    must("kIrSoaColumnSlabArenaUpstreamIssue = 4263", "AC1 stamp", soa)
    must("IrSoaSlabUpstream", "AC1 upstream type", soa)
    must("struct IrSoaColumnSlab", "AC1 slab", soa)
    must("&upstream", "AC1 bind upstream", soa)

    slab_pos = soa.find("struct IrSoaColumnSlab")
    if slab_pos < 0:
        fails.append("AC1: IrSoaColumnSlab missing")
    else:
        win = soa[slab_pos : slab_pos + 800]
        win_code = _strip_comments(win)
        if "new_delete_resource()" in win_code:
            fails.append("AC1: IrSoaColumnSlab still upstreams new_delete")
        if "&upstream" not in win:
            fails.append("AC1: resource must bind &upstream")

    # ── AC2: BMI pins + bind ──
    must("offsetof(IRFunctionSoA, block_dirty_) == 376", "AC2 BMI", soa)
    must("offsetof(IRFunctionSoA, instruction_dirty_) == 408", "AC2 BMI", soa)
    must("offsetof(IRFunctionSoA, generation_) == 440", "AC2 BMI", soa)
    must("sizeof(IRFunctionSoA) == 448", "AC2 BMI", soa)
    must("bind_column_arena", "AC2 bind", soa)
    must("kIrSoaColumnArenaIssue = 3833", "AC2 #3833 kept", soa)
    must("kIrSoaColumnSlabShardIssue = 3853", "AC2 #3853 kept", soa)

    # ── AC3: tests + wiring + no invent ──
    must("4263 AC1", "AC3 test", test)
    must("4263 AC2", "AC3 test", test)
    must("4263 AC3", "AC3 test", test)
    must("kIrSoaColumnSlabArenaUpstreamIssue", "AC3 test stamp", test)
    must("IrSoaSlabUpstream", "AC3 test cite", test)
    must(LINTER, "AC3 build.py", build)
    must("Issue #4263", "AC3 build cite", build)
    must(LINTER + ".py", "AC3 grandfather", gf)
    must('"issue": 4263', "AC3 manifest", man)

    must_not("g_4263_", "AC3 no counter invent", soa_code)
    if (ROOT / "tests/compiler/test_issue_4263.cpp").is_file():
        fails.append("AC3: test_issue_4263.cpp invent forbidden")
    if (ROOT / "docs/design/4263-column-slab-upstream.md").is_file():
        fails.append("AC3: docs/design invent forbidden")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: #4263 IrSoaColumnSlab Arena-owned upstream — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
