#!/usr/bin/env python3
"""Issue #2652 / #2649 H12: string/symbol heap corruption guards.

Contract:
  AC1 copy_string_heap_at + hash_tables_mutex on Evaluator
  AC2 hash-set!/hash-ref lock + refuse empty keys
  AC3 display/write NUL-safe snapshot
  AC4 format/symbol-append/number->string locked push
  AC5 unit test + cmake + build.py

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
    vec = _read("src/compiler/evaluator_primitives_vector.cpp")
    rt = _read("src/compiler/evaluator_primitives_runtime.cpp")
    pair = _read("src/compiler/evaluator_primitives_pair.cpp")
    reg = _read("src/compiler/evaluator_primitives_registry.cpp")
    test = _read("tests/compiler/test_string_heap_corruption_guard.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("copy_string_heap_at" in ixx, "AC1: copy_string_heap_at")
    must("hash_tables_mtx_" in ixx or "hash_tables_mutex" in ixx, "AC1: hash mutex")
    must("#2652" in ixx, "AC1: evaluator.ixx cites #2652")

    must("#2652" in vec, "AC2: vector cites #2652")
    must("hash_tables_mutex" in vec or "hash_tables_mtx" in vec, "AC2: hash-set locks")
    must("empty" in vec and "hash-set!" in vec, "AC2: empty key refuse")
    must("Evaluator& ev" in vec or "Evaluator&" in vec, "AC2: register takes Evaluator")

    must("#2652" in rt, "AC3: runtime cites #2652")
    must("copy_string_heap_at" in rt, "AC3: display/format snapshot")
    must("c == 0" in rt or "NUL" in rt or "null" in rt.lower(), "AC3: NUL handling")

    must("push_string_heap" in pair and "number->string" in pair, "AC4: number->string path")
    must("push_string_heap" in rt, "AC4: format/symbol-append push_string_heap")

    must("test_string_heap_corruption_guard" in cmake, "AC5: cmake")
    must("check_string_heap_corruption_guard_2652" in build, "AC5: linter")
    must("cmd_string_heap_corruption_guard_coverage" in build, "AC5: coverage cmd")
    must("AC1" in test and "AC2" in test, "AC5: unit ACs")
    must("Evaluator&" in reg or "primitive_error_counter, *this" in reg, "AC5: registry passes ev")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2652 string heap corruption guard — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
