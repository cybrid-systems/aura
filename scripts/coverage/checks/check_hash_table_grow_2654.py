#!/usr/bin/env python3
"""Issue #2654: language (hash) / hash-set! grow FlatHashTable.

Contract:
  AC1 flat_hash_grow_eval + flat_hash_insert_eval helpers
  AC2 load-factor 0.7 grow; hash and hash-set! use insert helper
  AC3 unit test + cmake + build.py gate

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

    vec = _read("src/compiler/evaluator_primitives_vector.cpp")
    test = _read("tests/compiler/test_hash_table_grow.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2654" in vec, "AC1: vector cites #2654")
    must("flat_hash_grow_eval" in vec, "AC1: grow helper")
    must("flat_hash_insert_eval" in vec, "AC1: insert helper")
    must("eval_hash_key_hash" in vec, "AC1: key hash helper")

    must("capacity * 7" in vec or "size * 10" in vec, "AC2: 0.7 load factor")
    must("flat_hash_insert_eval" in vec and 'add("hash"' in vec, "AC2: hash uses insert")
    must('add("hash-set!"' in vec and "flat_hash_insert_eval" in vec, "AC2: hash-set! uses insert")
    must("g_hash_tables[hidx] = ht" in vec, "AC2: publish grown pointer")

    must("test_hash_table_grow" in cmake, "AC3: cmake")
    must("check_hash_table_grow_2654" in build, "AC3: linter")
    must("cmd_hash_table_grow_coverage" in build, "AC3: coverage cmd")
    must("AC1" in test and "AC2" in test and "#2654" in test, "AC3: unit ACs")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2654 language hash table grow — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
