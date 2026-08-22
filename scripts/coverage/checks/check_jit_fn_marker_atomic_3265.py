#!/usr/bin/env python3
"""Issue #3265: atomic JIT fn marker/provenance tables + arena mark stack.

g_jit_fn_source_marker / g_jit_fn_provenance are std::atomic arrays.
Stamp uses CAS (release) so compile vs reset cannot tear marker vs
provenance. Accessors acquire-load. tl_arena_alloc reuses `aligned`
(no aligned2). tl_arena_push/pop use TLarena::marks[] so intervening
allocs cannot scramble offset. Soft unmatched pop is a no-op.

Contract:
  AC1  marker/provenance arrays are atomic
  AC2  stamp CAS + acquire readers; reset store(0, relaxed)
  AC3  tl_arena_alloc has no aligned2
  AC4  mark stack; unmatched pop does not scramble (zero extra)
  AC5  extend existing suites; linter after #3264; no invent

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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    hdr = _read("src/compiler/runtime_shared.h")
    test = _read("tests/compiler/test_jit_macro_introduced_preserve.cpp")
    arena = _read("tests/core/test_arena_lifecycle.cpp")
    build = _read("build.py")
    l3264 = _read("scripts/coverage/checks/check_cascade_dep_graph_atomic_3264.py")

    must("Issue #3265", "AC1 cite", rt)
    must("std::atomic<std::uint8_t> g_jit_fn_source_marker", "AC1 marker", rt)
    must("std::atomic<std::uint32_t> g_jit_fn_provenance", "AC1 provenance", rt)
    must("ac3265_1_atomic_side_table", "AC1 test", test)

    spos = rt.find("void aura_jit_stamp_fn_macro_marker")
    swin = rt[spos : spos + 2200] if spos >= 0 else ""
    must("compare_exchange_weak", "AC2 CAS stamp", swin)
    must("compare_exchange_strong", "AC2 CAS clear", swin)
    must("memory_order_release", "AC2 release", swin)
    rpos = rt.find("uint8_t aura_jit_fn_source_marker")
    rwin = rt[rpos : rpos + 500] if rpos >= 0 else ""
    must("memory_order_acquire", "AC2 accessor acquire", rwin)
    zpos = rt.find("void aura_counters_reset()")
    zwin = rt[zpos : zpos + 1600] if zpos >= 0 else ""
    must("store(0, std::memory_order_relaxed)", "AC2 reset relaxed", zwin)
    must("ac3265_2_stamp_acquire_roundtrip", "AC2 test", test)

    apos = rt.find("void* tl_arena_alloc(")
    awin = rt[apos : apos + 1600] if apos >= 0 else ""
    if "aligned2" in awin:
        fails.append("AC3: aligned2 recompute still present")
    must("arena->base + aligned", "AC3 reuse", awin)
    must("ac3265_3_alloc_no_aligned2", "AC3 test", test)

    must("kMaxMarks", "AC4 header", hdr)
    must("marks[kMaxMarks]", "AC4 marks", hdr)
    must("mark_depth", "AC4 depth", hdr)
    ppos = rt.find("void tl_arena_pop(TLarena* arena)")
    pwin = rt[ppos : ppos + 900] if ppos >= 0 else ""
    must("marks[--arena->mark_depth]", "AC4 pop marks", pwin)
    if "base + arena->offset))[-1]" in pwin:
        fails.append("AC4: pop still reads arena memory")
    must("ac3265_4_mark_stack_intervening_alloc", "AC4 test", test)

    must("ac3265_5_source_and_linter", "AC5 test", test)
    must("3265", "AC5 arena family", arena)
    must("check_jit_fn_marker_atomic_3265", "AC5 build.py", build)
    prev = build.find("check_cascade_dep_graph_atomic_3264")
    ours = build.find("check_jit_fn_marker_atomic_3265")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3264")
    must("3265", "AC5 extend 3264 linter", l3264)
    if (ROOT / "tests" / "issues" / "test_issue_3265.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3265.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3265.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3265.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3265-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3265" in q or "schema-3265" in test:
        fails.append("AC5: new schema-3265 query key (SlimSurface)")

    if fails:
        print("FAIL #3265 jit_fn_marker_atomic:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3265 jit_fn_marker_atomic: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
