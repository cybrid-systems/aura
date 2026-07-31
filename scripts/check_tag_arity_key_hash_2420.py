#!/usr/bin/env python3
"""Issue #2420: TagArityKeyHash pack + splitmix finalizer.

Contract:
  AC1 pack tag/arity into uint64 + splitmix constants
  AC2 collision test present (≤1% at 10K)
  AC3 no legacy FNV-1a loop as sole hash
  AC4 gate + test wired

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_tag_arity_key_hash_2420.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate TagArityKeyHash body
    idx = ast.find("struct TagArityKeyHash")
    body = ast[idx : idx + 900] if idx >= 0 else ""

    must("Issue #2420", "AC1", ast)
    must("splitmix", "AC1", body)
    must("0xbf58476d1ce4e5b9ull", "AC1", body)
    must("0x94d049bb133111ebull", "AC1", body)
    must("<< 32", "AC1", body)
    must("2420 AC1", "AC1", test)

    must("2420 AC2", "AC2", test)
    must("collision", "AC2", test)

    # Old FNV-only form should not remain as production hash body
    must_not("14695981039346656037ull", "AC3", body)
    must("2420 AC3", "AC3", test)

    must("2420 AC4", "AC4", test)
    must("check_tag_arity_key_hash_2420", "gate", build)
    must("cmd_tag_arity_key_hash_coverage", "gate", build)
    must("test_tag_arity_key_hash_2420", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: tag_arity_key_hash #2420 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
