#!/usr/bin/env python3
"""Issue #3269: arena compact/defrag TOCTOU — unique workspace after TLS skip.

any_active_mutation_boundary is TLS-only (same-fiber). Compact must then
take WorkspaceUniqueIfNeeded so in-flight Guards drain and new Guards
cannot enter. Same-fiber Guard still pauses (non-recursive unique).
Raced-after-check counter records exclusive miss (should stay 0).
live-compact process-counter snapshot is documented best-effort.

Contract:
  AC1  four primitives use with_arena_compact_idle + unique workspace
  AC2  raced-after-check counter; re-check mutation_boundary_held_
  AC3  live-compact snapshot best-effort comment; relaxed kept
  AC4  same-fiber Guard pauses compact (zero extra idle unique)
  AC5  extend test_arena_defrag; linter after #3268; no invent

Exit 0 = all rows satisfied.

Follow-up #3270: exhaustive PrimId drift asserts + OpGuardShape probe i1.
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

    mem = _read("src/compiler/evaluator_primitives_memory.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    compile_p = _read("src/compiler/evaluator_primitives_compile.cpp")
    test = _read("tests/core/test_arena_defrag.cpp")
    build = _read("build.py")
    l3268 = _read("scripts/coverage/checks/check_guard_flag_atomic_ref_3268.py")

    must("Issue #3269", "AC1 cite", mem)
    must("with_arena_compact_idle", "AC1 helper", mem)
    must("WorkspaceUniqueIfNeeded compact_hold", "AC1 unique", mem)
    for name in (
        'add("arena:compact"',
        'sink_arena_prim("arena:defrag"',
        'sink_arena_prim("arena:defrag-now"',
        'sink_arena_prim("arena:live-compact"',
    ):
        pos = mem.find(name)
        win = mem[pos : pos + 900] if pos >= 0 else ""
        must("with_arena_compact_idle", f"AC1 {name} helper", win)
    must("ac3269_1_unique_after_tls_skip", "AC1 test", test)

    must("compaction_boundary_raced_after_check_", "AC2 member", ixx)
    must("compaction_boundary_raced_after_check_", "AC2 bump", mem)
    must("mutation_boundary_held()", "AC2 re-check", mem)
    must("compaction-boundary-raced-after-check", "AC2 query additive", compile_p)
    must("ac3269_2_raced_counter", "AC2 test", test)

    lpos = mem.find('sink_arena_prim("arena:live-compact"')
    lwin = mem[lpos : lpos + 1800] if lpos >= 0 else ""
    must("best-effort snapshot", "AC3 comment", lwin)
    must("memory_order_relaxed", "AC3 relaxed", lwin)
    must("ac3269_3_snapshot_comment", "AC3 test", test)

    must("any_active_mutation_boundary()", "AC4 TLS skip", mem)
    must("ac3269_4_compact_under_guard", "AC4 test", test)

    must("ac3269_5_source_and_linter", "AC5 test", test)
    must("check_arena_compact_toctou_3269", "AC5 build.py", build)
    prev = build.find("check_guard_flag_atomic_ref_3268")
    ours = build.find("check_arena_compact_toctou_3269")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3268")
    must("3269", "AC5 extend 3268 linter", l3268)
    if (ROOT / "tests" / "issues" / "test_issue_3269.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3269.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3269.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3269.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3269-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3269" in q or "schema-3269" in test:
        fails.append("AC5: new schema-3269 query key (SlimSurface)")

    if fails:
        print("FAIL #3269 arena_compact_toctou:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3269 arena_compact_toctou: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
