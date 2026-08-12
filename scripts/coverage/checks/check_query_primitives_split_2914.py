#!/usr/bin/env python3
"""Issue #2914: query primitives modularization + error convention.

AC:
  1. evaluator_primitives_query.cpp split; peels present under LOC budget
  2. register_query_primitives orchestrates peels
  3. Error convention doc present
  4. CMake lists peels
  5. No single peel >> 5k LOC (soft ceiling; target ~2-3k)
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOFT_CAP = 4500  # soft; peels may sit slightly over 3k for natural cuts


def main() -> int:
    fails = []
    main_q = ROOT / "src/compiler/evaluator_primitives_query.cpp"
    peels = [
        "evaluator_primitives_query_obs_mid.cpp",
        "evaluator_primitives_query_type_stats.cpp",
        "evaluator_primitives_query_reflect.cpp",
        "evaluator_primitives_query_lifecycle.cpp",
        "evaluator_primitives_query_tail.cpp",
    ]
    if not main_q.is_file():
        fails.append("main query.cpp missing")
    else:
        n = sum(1 for _ in main_q.open())
        if n > SOFT_CAP:
            fails.append(f"main query.cpp still {n} LOC (cap {SOFT_CAP})")
        t = main_q.read_text(encoding="utf-8", errors="replace")
        for p in peels:
            stem = p.replace("evaluator_primitives_query_", "").replace(".cpp", "")
            if f"register_query_{stem}_primitives" not in t:
                fails.append(f"main missing call/decl for {stem}")
        if "Issue #2914" not in t:
            fails.append("main missing #2914 cite")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    for p in peels:
        path = ROOT / "src/compiler" / p
        if not path.is_file():
            fails.append(f"missing peel {p}")
            continue
        n = sum(1 for _ in path.open())
        if n > 5500:
            fails.append(f"{p} is {n} LOC (hard cap 5500)")
        if f"src/compiler/{p}" not in cmake:
            fails.append(f"CMakeLists missing {p}")
        pt = path.read_text(encoding="utf-8", errors="replace")
        if "Issue #2914" not in pt:
            fails.append(f"{p} missing #2914 cite")

    doc = ROOT / "docs/stdlib/primitive-error-convention.md"
    if not doc.is_file():
        fails.append("missing docs/stdlib/primitive-error-convention.md")
    else:
        d = doc.read_text(encoding="utf-8", errors="replace")
        if "make_primitive_error" not in d or "2914" not in d:
            fails.append("error convention doc incomplete")

    shared = ROOT / "src/compiler/evaluator_primitives_query_shared.hh"
    if not shared.is_file():
        fails.append("missing evaluator_primitives_query_shared.hh")

    # Spot-check: list-ref still uses make_primitive_error for true errors
    lst = (ROOT / "src/compiler/evaluator_primitives_list.cpp").read_text(encoding="utf-8", errors="replace")
    if "list-ref" in lst and "make_primitive_error" not in lst:
        fails.append("list-ref lost make_primitive_error")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2914 query primitives split + error convention — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
