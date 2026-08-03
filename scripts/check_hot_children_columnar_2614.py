#!/usr/bin/env python3
"""Issue #2614: force ChildColumnar/SoAColumnarFull on walk/query/PCV hot templates.

Contract:
  AC1 walk_children_column / walk_children_hot / ChildrenColumnarProvider
  AC2 NonColumnarChildren fails ChildColumnar (test static_assert)
  AC3 constexpr requires-only helpers (no runtime gate)
  AC4 source-cite on ast/concepts/pcv/query + static_assert PCV shape
  AC5 test + cmake + build.py gate; no design docs

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

    concepts = _read("src/core/concepts.ixx")
    ast = _read("src/core/ast.ixx")
    pcv = _read("src/core/persistent_child_vector.hh")
    query = _read("src/compiler/query.ixx")
    test = _read("tests/core/test_hot_children_columnar_2614.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("ChildrenColumnarProvider", "AC1", concepts)
    must("walk_children_column", "AC1", concepts)
    must("ChildColumnar<Col>", "AC1", concepts)
    must("walk_children_hot", "AC1", ast)
    must("ChildrenColumnarProvider", "AC1", ast)
    must("children_columnar", "AC1", query)
    must("walk_children_hot", "AC1", query)
    must("ac1_constrained", "AC1", test)

    # AC2
    must("NonColumnarChildren", "AC2", test)
    must("!ChildColumnar<NonColumnarChildren>", "AC2", test)
    must("ac2_non_columnar_isolated", "AC2", test)

    # AC3
    must("constexpr std::size_t walk_children_column", "AC3", concepts)
    must("ac3_zero_runtime", "AC3", test)

    # AC4
    must("Issue #2614", "AC4", concepts)
    must("Issue #2614", "AC4", ast)
    must("Issue #2614", "AC4", pcv)
    must("safe_pcv_child_columnar_shape", "AC4", pcv)
    must("ChildColumnar<SafePCVSpan", "AC4", ast)
    must("SoAColumnarFull<SafePCVSpan", "AC4", ast)
    must("ac4_source_cite", "AC4", test)

    # AC5 gate
    must("test_hot_children_columnar_2614", "AC5", cmake)
    must("check_hot_children_columnar_2614", "AC5", build)
    must("cmd_hot_children_columnar_coverage", "AC5", build)
    must("ac5_walk_smoke", "AC5", test)
    for rel in (
        "docs/design/hot_children_columnar_2614.md",
        "docs/hot_children_columnar_2614.md",
        "design/2614.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2614 hot children ChildColumnar force — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
