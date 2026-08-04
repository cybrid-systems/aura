#!/usr/bin/env python3
"""Issue #2410: meta(NodeTag) bounds-checked OOB sentinel.

Contract:
  AC1 meta(0) / NodeTag{} → sentinel
  AC2 meta(0xFF) → sentinel
  AC3 valid tags preserved
  AC4 ASan-safe (no OOB index)
  AC5 static_assert table size == Class

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

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_node_meta_bounds.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate meta() body
    idx = ast.find("export constexpr const NodeMeta& meta(NodeTag tag)")
    if idx < 0:
        fails.append("AC1: meta() not found")
        body = ""
    else:
        body = ast[idx : idx + 500]

    must("Issue #2410", "AC1", ast)
    must("kNodeTagMax", "AC1", ast)
    must("is_valid_node_tag", "AC1", ast)
    must("idx == 0", "AC1", body)
    must("kNodeMeta[0]", "AC1", body)
    must("2410 AC1", "AC1", test)

    must("0xFF", "AC2", ast)
    must("2410 AC2", "AC2", test)

    must("NodeTag::Class", "AC3", ast)
    must("2410 AC3", "AC3", test)

    # Old unbounded form should not remain as sole return
    if "return kNodeMeta[static_cast<std::size_t>(tag) - 1];" in body and "if (idx == 0" not in body:
        fails.append("AC4: meta() still unbounded index-only return")

    must("static_assert(kNodeMeta.size() == static_cast<std::size_t>(NodeTag::Class)", "AC5", ast)
    must("2410 AC5", "AC5", test)

    must("check_node_meta_bounds_2410", "gate", build)
    must("cmd_node_meta_bounds_coverage", "gate", build)
    must("test_node_meta_bounds", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: node meta bounds #2410 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
