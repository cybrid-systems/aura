#!/usr/bin/env python3
"""Issue #2802: replace-pattern pattern flat/pool on local ASTArena (not temp_arena_).

Sibling replace-pattern sub-ops in atomic-batch must not share Evaluator
temp_arena_ memory for pattern matching. Per-call pat_arena + metric.

Contract (one row per AC):
  AC1 public + lockless cite #2802; ASTArena pat_arena; no temp_arena_->create for pattern
  AC2 replace_pattern_temp_arena_corruption_prevented metric on FlatAST
  AC3 tests/compiler/test_atomic_batch_replace_pattern_sibling.cpp
  AC4 this linter wired; no docs/design/2802-*; no test_issue_2802.cpp

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


def _pat_window(src: str, anchor: str, size: int) -> str:
    pos = src.find(anchor)
    if pos < 0:
        return ""
    return src[pos : pos + size]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ast = _read("src/core/ast.ixx")
    test = _read("tests/compiler/test_atomic_batch_replace_pattern_sibling.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    ppos = mut.find('add_mutate("mutate:replace-pattern"')
    if ppos < 0:
        ppos = mut.find("mutate:replace-pattern")
    pwin = mut[ppos : ppos + 12000] if ppos >= 0 else ""

    # Body includes phase-2 nest-safe batch (~phase 1 + apply ~8KB).
    lwin = _pat_window(flat, "eval_flat_apply_mutate_replace_pattern", 10000)

    # AC1
    must("Issue #2802", "AC1", pwin)
    must("ASTArena pat_arena", "AC1", pwin)
    must("note_replace_pattern_temp_arena_corruption_prevented", "AC1", pwin)
    if "temp_arena_->create" in pwin:
        fails.append("AC1: public still uses temp_arena_->create in replace-pattern window")
    if "temp_arena_->allocator" in pwin:
        fails.append("AC1: public still uses temp_arena_->allocator in replace-pattern window")

    must("Issue #2802", "AC1", lwin)
    must("ASTArena pat_arena", "AC1", lwin)
    must("note_replace_pattern_temp_arena_corruption_prevented", "AC1", lwin)
    if "atomic_batch_active" not in lwin and "nested_outer_batch" not in lwin:
        fails.append("AC1: lockless missing nest-safe atomic_batch_active / nested_outer_batch")
    if "temp_arena_->create" in lwin:
        fails.append("AC1: lockless still uses temp_arena_->create in replace-pattern window")
    if "temp_arena_->allocator" in lwin:
        fails.append("AC1: lockless still uses temp_arena_->allocator in replace-pattern window")

    # AC2
    must("replace_pattern_temp_arena_corruption_prevented", "AC2", ast)
    must("Issue #2802", "AC2", ast)
    must("note_replace_pattern_temp_arena_corruption_prevented", "AC2", ast)

    # AC3
    must("ac2802", "AC3", test)
    must("2802", "AC3", test)
    must("pat_arena", "AC3", test)
    must("replace_pattern_temp_arena_corruption_prevented", "AC3", test)
    must("atomic-batch", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_atomic_batch_replace_pattern_sibling.cpp").is_file():
        fails.append("AC3: missing test_atomic_batch_replace_pattern_sibling.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2802.cpp").is_file():
        fails.append("AC3: test_issue_2802.cpp present (forbidden per #81967)")
    must("test_atomic_batch_replace_pattern_sibling", "AC3", cmake)

    # AC4
    must("check_atomic_batch_replace_pattern_sibling_2802", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2802-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2802 replace-pattern sibling isolation — local ASTArena pat_arena + corruption-prevented metric")
    return 0


if __name__ == "__main__":
    sys.exit(main())
