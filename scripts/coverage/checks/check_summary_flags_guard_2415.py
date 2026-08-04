#!/usr/bin/env python3
"""Issue #2415: summary_flags_ thread-safety annotation + FlatAST audit.

Contract:
  AC1 summary_flags_ documents GUARDED_BY N/A (atomic #2463), not plain uint32_t
  AC2 free_list_ has GUARDED_BY(flatast_mutex_) audit comment
  AC3 SoA size-mutation GUARDED_BY audit present
  AC4 gate + test wired

Note: original issue text assumed non-atomic summary_flags_; #2463 already
made it atomic. This check enforces the corrected annotation model.

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
    test = _read("tests/core/test_summary_flags_guard.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate summary_flags_ declaration region
    idx = ast.find("summary_flags_{0}")
    if idx < 0:
        idx = ast.find("summary_flags_")
    region = ast[max(0, idx - 900) : idx + 200] if idx >= 0 else ""

    must("Issue #2415", "AC1", region)
    must("GUARDED_BY(flatast_mutex_)", "AC1", region)
    must("N/A", "AC1", region)
    must("std::atomic<std::uint32_t> summary_flags_", "AC1", ast)
    # Must not reintroduce plain non-atomic summary_flags_
    must_not("std::uint32_t summary_flags_ = 0", "AC1", ast)
    must_not("std::uint32_t summary_flags_ GUARDED_BY", "AC1", ast)
    must("2415 AC1", "AC1", test)

    must("GUARDED_BY(flatast_mutex_)", "AC2", ast)
    must("free_list_", "AC2", ast)
    # free_list annotation nearby
    fl = ast.find("std::pmr::vector<NodeId> free_list_")
    fl_region = ast[max(0, fl - 600) : fl + 80] if fl >= 0 else ""
    must("GUARDED_BY(flatast_mutex_)", "AC2", fl_region)
    must("2415 AC2", "AC2", test)

    must("Issue #2415 audit", "AC3", ast)
    must("SoA storage", "AC3", ast)
    must("2415 AC3", "AC3", test)

    must("2415 AC4", "AC4", test)
    must("check_summary_flags_guard_2415", "gate", build)
    must("cmd_summary_flags_guard_coverage", "gate", build)
    must("test_summary_flags_guard", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: summary_flags guard #2415 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
