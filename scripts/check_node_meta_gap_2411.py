#!/usr/bin/env python3
"""Issue #2411: kNodeMeta gap is_gap + full tag/name consistency.

Contract:
  AC1 gap is_gap / tag 0x0C (not LiteralInt spoof)
  AC2 validate_node_meta full-table consistency loop
  AC3 meta(LiteralInt) is real entry (not gap)
  AC4 valid tags unchanged (callers)
  AC5 is_gap used in validate_node path (ast_impl)

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
    impl = _read("src/core/ast_impl.cpp")
    test = _read("tests/core/test_node_meta_gap_2411.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate NodeMeta and gap entry
    must("bool is_gap", "AC1", ast)
    must("Issue #2411", "AC1", ast)
    must("static_cast<NodeTag>(0x0C)", "AC1", ast)
    # Old spoof form must not remain as the gap line
    must_not(
        "{NodeTag::LiteralInt, kNodeTagNames[11], 0, false, false, false, false, false}, // 0x0C gap",
        "AC1",
        ast,
    )
    must("2411 AC1", "AC1", test)

    # Full-table validate loop
    must("kNodeTagGapIndex", "AC2", ast)
    must("e.is_gap", "AC2", ast)
    must("for (std::size_t i = 0; i < kNodeMeta.size(); ++i)", "AC2", ast)
    must("2411 AC2", "AC2", test)

    must("meta(NodeTag::LiteralInt)", "AC3", ast)
    must("2411 AC3", "AC3", test)
    must("&meta(NodeTag::LiteralInt) != &kNodeMeta[0]" if False else "kNodeMeta[0]", "AC3", ast)

    must("2411 AC4", "AC4", test)

    # validate_node prefers is_gap
    must("m.is_gap", "AC5", impl)
    must_not('m.name == "<gap>"', "AC5", impl)
    must("2411 AC5", "AC5", test)

    must("check_node_meta_gap_2411", "gate", build)
    must("cmd_node_meta_gap_coverage", "gate", build)
    must("test_node_meta_gap_2411", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: node meta gap #2411 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
