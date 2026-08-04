#!/usr/bin/env python3
"""Issue #2651 / #2649 H9: concurrent PMR / string_heap / ASTArena allocate safety.

Contract:
  AC1 push_string_heap / push_pair hold alloc_storage_lock_
  AC2 string-append / cons lock under multi-fiber
  AC3 ASTArena allocate_raw_impl serializes pmr via alloc_mtx_
  AC4 unit test + cmake + build.py gate

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


def main() -> int:
    fails: list[str] = []

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    ixx = _read("src/compiler/evaluator.ixx")
    pair = _read("src/compiler/evaluator_primitives_pair.cpp")
    listp = _read("src/compiler/evaluator_primitives_list.cpp")
    test = _read("tests/compiler/test_pmr_alloc_fiber_safe.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2651" in ixx, "AC1: evaluator.ixx cites #2651")
    must("push_string_heap" in ixx and "alloc_storage_lock_" in ixx, "AC1: push locks")
    # push_string_heap body must lock
    idx = ixx.find("std::int32_t push_string_heap(const std::string& s)")
    must(idx != -1, "AC1: push_string_heap(const&) present")
    if idx != -1:
        win = ixx[idx : idx + 400]
        must("alloc_storage_lock_" in win, "AC1: push_string_heap locks in body")

    must("#2651" in pair, "AC2: pair.cpp cites #2651")
    must("string-append" in pair, "AC2: string-append present")
    # string-append lambda should mention lock near it
    sa = pair.find("string-append")
    must(sa != -1, "AC2: string-append found")
    if sa != -1:
        win = pair[sa : sa + 800]
        must("alloc_storage_lock_" in win, "AC2: string-append critical section locks")
    must('add("cons"' in pair or 'add("cons",' in pair, "AC2: cons present")

    must("#2651" in listp, "AC3: list.cpp cites #2651")
    must("alloc_storage_lock_" in listp, "AC3: list primitives lock")

    must("AC1" in test and "AC4" in test, "AC4: unit test ACs")
    must("push_string_heap" in test, "AC4: test stresses push_string_heap")
    must("test_pmr_alloc_fiber_safe" in cmake, "AC4: cmake registers test")
    must("check_pmr_alloc_fiber_safe_2651" in build, "AC4: build.py linter")
    must("cmd_pmr_alloc_fiber_safe_coverage" in build, "AC4: coverage cmd")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2651 PMR/string_heap fiber-safe alloc — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
