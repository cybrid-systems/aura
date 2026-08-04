#!/usr/bin/env python3
"""Issue #2414: summary_recompute(pool) restores sym_id summary bits.

Contract:
  AC1 summary_recompute takes StringPool* and ORs HasKeywordVar / query-mutate
  AC2 Call callee Variable path for query:/mutate:
  AC3 default nullptr overload still present (tag/int only)
  AC4 test covers keyword + query cases
  AC5 gate wired

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
    test = _read("tests/core/test_summary_recompute_sym_2414.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    idx = ast.find("void summary_recompute(")
    if idx < 0:
        fails.append("AC1: summary_recompute not found")
        body = ""
    else:
        body = ast[idx : idx + 1800]

    must("Issue #2414", "AC1", ast)
    must("const StringPool* pool", "AC1", body)
    must("summary_flags_for_sym_id", "AC1", body)
    must("HasKeywordVar", "AC1", ast)
    must("HasQueryOrMutateCall", "AC1", ast)
    must("2414 AC1", "AC1", test)

    must("NodeTag::Call", "AC2", body)
    must("query:", "AC2", body)
    must("2414 AC2", "AC2", test)

    must("pool = nullptr", "AC3", body)
    # Old skip-only comment should not be the sole approach
    must_not(
        "require pool resolution and are NOT\n            // recomputed here",
        "AC3",
        body,
    )
    must("2414 AC3", "AC3", test)

    must("2414 AC4", "AC4", test)
    must("2414 AC5", "AC5", test)
    must(":mode", "AC5", test)
    must("query:find", "AC5", test)

    must("check_summary_recompute_sym_2414", "gate", build)
    must("cmd_summary_recompute_sym_coverage", "gate", build)
    must("test_summary_recompute_sym_2414", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: summary_recompute sym #2414 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
