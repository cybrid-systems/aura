#!/usr/bin/env python3
"""Issue #2418: structural_mtx_ → metadata_mtx_ canonical lock order.

Contract:
  AC1 documented LOCK ORDER structural before metadata
  AC2 ACQUIRES annotations on set_child / set_marker / set_provenance
  AC3 CombinedStructuralMetadataWriteGuard present
  AC4 concurrent dual-hold test
  AC5 audit: no metadata-then-structural reverse pattern in src/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _audit_reverse_order() -> list[str]:
    """Flag src files that take metadata write then structural in same function.

    Heuristic: begin_metadata_mutation appears before begin_structural_mutation
    within a 2k-char window — known deadlocked nesting pattern.
    """
    hits: list[str] = []
    for p in (ROOT / "src").rglob("*"):
        if p.suffix not in {".ixx", ".cpp", ".hpp", ".h", ".hh"}:
            continue
        try:
            t = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        # Skip the combined guard definition itself and docs.
        if "CombinedStructuralMetadataWriteGuard" in t and "begin_structural_and_metadata" in t:
            # Still check other sites in file
            pass
        for m in re.finditer(r"begin_metadata_mutation\s*\(", t):
            window = t[m.start() : m.start() + 2000]
            if "begin_structural_mutation" in window:
                # Allow if combined helper is used instead
                if "begin_structural_and_metadata_mutation" in window:
                    continue
                # If structural appears only in comments...
                if re.search(r"begin_structural_mutation\s*\(", window):
                    rel = str(p.relative_to(ROOT))
                    hits.append(f"{rel}: metadata then structural nest risk near offset {m.start()}")
    return hits


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_structural_metadata_lock_order_2418.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2418", "AC1", ast)
    must("LOCK ORDER", "AC1", ast)
    must("structural_mtx_ BEFORE metadata_mtx_", "AC1", ast)
    must("2418 AC1", "AC1", test)

    must("ACQUIRES(structural_mtx_)", "AC2", ast)
    must("REQUIRES metadata write lock", "AC2", ast)
    must("2418 AC2", "AC2", test)

    must("CombinedStructuralMetadataWriteGuard", "AC3", ast)
    must("begin_structural_and_metadata_mutation", "AC3", ast)
    must("2418 AC3", "AC3", test)

    must("2418 AC4", "AC4", test)
    must("2418 AC5", "AC5", test)

    reverse = _audit_reverse_order()
    if reverse:
        for h in reverse:
            fails.append(f"AC3-audit: {h}")
    else:
        print("=== #2418 AC3 audit: no metadata→structural nest found in src/ ===")

    must("check_structural_metadata_lock_order_2418", "gate", build)
    must("cmd_structural_metadata_lock_order_coverage", "gate", build)
    must("test_structural_metadata_lock_order_2418", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: structural/metadata lock order #2418 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
