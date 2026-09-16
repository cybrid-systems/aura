#!/usr/bin/env python3
"""Issue #3853: IR SoA column_slab map sharded (no process-wide mu).

#3833 moved IR instruction columns to 24B IrSoaArenaColumn with
per-function monotonic slabs, but the side map that owns those slabs
was guarded by a single process-wide std::mutex — concurrent lower /
batch-dirty / dual-emit across fibers serialized on every bind/lookup.

Fix: shard the map by hash(key) % N (mirror ShapeProfiler FnKey
shards). Keep 24B column BMI pins. Heap upstream past 8KiB seed is a
MED residual — not required this PR.

Contract (one row per AC):
  AC1  no process-wide column_slab_mu for all keys; shards + shard_index
  AC2  #3833 24B BMI offsetof / sizeof pins still hold; bind path intact
  AC3  extends test_ir_soa_layout_stamp; linter + grandfather + manifest
       + build.py; no invent / docs/design; no g_3853_* counters

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
MANIFEST = "scripts/coverage/manifests/3853.json"
LINTER = "check_ir_soa_column_slab_shard_3853"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _strip_comments(src: str) -> str:
    # Drop // line comments so forbid checks ignore cites.
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

    # ── AC1: sharded map; process-wide single mu gone ──
    must("Issue #3853", "AC1 cite", soa)
    must("kIrSoaColumnSlabShardIssue = 3853", "AC1 stamp", soa)
    must("kIrSoaColumnSlabShardCount = 16", "AC1 shard count", soa)
    must("IrSoaColumnSlabShard", "AC1 shard type", soa)
    must("column_slab_shards()", "AC1 shards()", soa)
    must("column_slab_shard_index", "AC1 shard_index", soa)
    must("column_slab_for", "AC1 for retained", soa)
    must("drop_column_slab", "AC1 drop retained", soa)
    must("rekey_column_slab", "AC1 rekey retained", soa)
    # ShapeProfiler-style SplitMix cite / power-of-two friendly count.
    must("0xbf58476d1ce4e5b9ULL", "AC1 SplitMix mix", soa)

    must_not("inline std::mutex& column_slab_mu()", "AC1 no process-wide mu fn", soa_code)
    # No single static mutex owning the whole map (member mu on shard OK).
    if re.search(r"static\s+std::mutex\s+mu\s*;", soa_code):
        fails.append("AC1: static std::mutex mu still present (process-wide)")
    # column_slabs() unsharded map accessor must be gone.
    must_not("column_slabs()", "AC1 no unsharded column_slabs()", soa_code)
    # Each hot path locks a shard, not a global.
    for_body_start = soa.find("inline IrSoaColumnSlab& column_slab_for(const void* key)")
    if for_body_start < 0:
        fails.append("AC1: column_slab_for missing")
    else:
        for_body = soa[for_body_start : for_body_start + 500]
        must("column_slab_shards()[column_slab_shard_index(key)]", "AC1 for uses shard", for_body)
        must("shard.mu", "AC1 for locks shard.mu", for_body)

    # ── AC2: BMI pins + bind intact ──
    must("offsetof(IRFunctionSoA, block_dirty_) == 376", "AC2 BMI block_dirty_", soa)
    must("offsetof(IRFunctionSoA, instruction_dirty_) == 408", "AC2 BMI instruction_dirty_", soa)
    must("offsetof(IRFunctionSoA, generation_) == 440", "AC2 BMI generation_", soa)
    must("sizeof(IRFunctionSoA) == 448", "AC2 BMI sizeof", soa)
    must(
        "sizeof(IrSoaArenaColumn<std::uint32_t>) == sizeof(std::vector<std::uint32_t>)",
        "AC2 24B column BMI",
        soa,
    )
    must("bind_column_arena", "AC2 bind retained", soa)
    must("kIrSoaColumnArenaIssue = 3833", "AC2 #3833 stamp kept", soa)
    must("IrSoaArenaColumn", "AC2 column type kept", soa)

    # ── AC3: tests + wiring ──
    must("3853 AC1", "AC3 test AC1", test)
    must("3853 AC2", "AC3 test AC2", test)
    must("3853 AC3", "AC3 test AC3", test)
    must("kIrSoaColumnSlabShardIssue", "AC3 test constant", test)
    must("column_slab_shards()", "AC3 test shards cite", test)
    must("no process-wide column_slab_mu()", "AC3 test no global mu", test)
    must("uses_arena_resource()", "AC3 post-bind arena", test)

    must(LINTER, "AC3 build registration", build)
    must("Issue #3853", "AC3 build cite", build)
    must(f"{LINTER}.py", "AC3 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC3 grandfather path", gf)
    must('"issue": 3853', "AC3 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC3: {MANIFEST} missing")

    must_not("g_3853_", "AC3 no invented counter", soa)
    must_not("schema-3853", "AC3 no query key", soa)
    must_not("g_3853_", "AC3 no invented counter test", test)

    for rel in (
        "tests/compiler/test_issue_3853.cpp",
        "tests/issues/test_issue_3853.cpp",
        "docs/design/3853-column-slab-shard.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC3: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3853*"):
            fails.append(f"AC3: docs/design/{p.name} exists")

    if fails:
        print("FAIL #3853 ir_soa_column_slab_shard:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(
        "OK #3853 ir_soa_column_slab_shard: sharded map; no process-wide mu; "
        "BMI pins kept"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
