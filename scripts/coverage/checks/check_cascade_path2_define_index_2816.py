#!/usr/bin/env python3
"""Issue #2816: cascade path2 uses O(N+M) define-by-sym index (not O(N×M)).

Contract (one row per AC):
  AC1 path2 builds define_by_sym; cites #2816; no per-name flat.size() scan
  AC2 cascade_path2_lookup/index metrics + query schema-2816
  AC3 test suite present
  AC4 this linter wired; no docs/design/2816-*; no test_issue_2816.cpp

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    met = _read("src/compiler/observability_metrics.h")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_cascade_path2_define_index.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    cascade = mut.find("push_post_mutate_incremental_cascade")
    p2816 = mut.find("Issue #2816", cascade if cascade >= 0 else 0)
    # Index build lives near #2816; metric fetch_add is later in the same cascade.
    end = mut.find("// 3) Eager partial re-lower", cascade if cascade >= 0 else 0)
    if end < 0:
        end = (cascade if cascade >= 0 else 0) + 9000
    cascade_body = mut[cascade:end] if cascade >= 0 else ""
    win = mut[p2816 : p2816 + 2200] if p2816 >= 0 else ""

    # AC1
    must("Issue #2816", "AC1", win)
    must("define_by_sym", "AC1", win)
    must("define_by_sym.find", "AC1", win)
    must("cascade_path2_lookup_total", "AC1", cascade_body)
    must("cascade_path2_index_nodes_total", "AC1", cascade_body)
    # Forbidden: per-name nested flat walk after index build (path2 name loop).
    # The name loop starts after path2_lookups increments.
    soft = mut.find("Soft IR-cache dirty", cascade if cascade >= 0 else 0)
    path2_block = mut[p2816:soft] if p2816 >= 0 and soft > p2816 else win
    name_loop = path2_block.find("for (const auto& n : defuse_affected_syms_)")
    if name_loop >= 0:
        name_body = path2_block[name_loop:]
        if "for (aura::ast::NodeId id = 0" in name_body:
            fails.append("AC1: per-name nested NodeId walk still present (O(N×M))")

    # AC2
    must("cascade_path2_lookup_total", "AC2", met)
    must("cascade_path2_index_nodes_total", "AC2", met)
    must("schema-2816", "AC2", obs)
    must("cascade_path2_lookup_total", "AC2", obs)

    # AC3
    must("ac2816", "AC3", test)
    must("2816", "AC3", test)
    must("define_by_sym", "AC3", test)
    must("cascade_path2_lookup_total", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_cascade_path2_define_index.cpp").is_file():
        fails.append("AC3: missing test_cascade_path2_define_index.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2816.cpp").is_file():
        fails.append("AC3: test_issue_2816.cpp present (forbidden per #81967)")
    must("test_cascade_path2_define_index", "AC3", cmake)

    # AC4
    must("check_cascade_path2_define_index_2816", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2816-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2816 cascade path2 define index — O(N+M) not O(N×M)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
