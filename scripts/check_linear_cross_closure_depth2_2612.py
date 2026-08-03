#!/usr/bin/env python3
"""Issue #2612: optional depth-2 cross-closure free-capture discovery.

Contract:
  AC1 default depth 1 via linear_cross_closure_depth_cap; nested skip retained
  AC2 depth 2 nested entry (depth2_entries / depth2_escape_sites) + hard_enabled
  AC3 cone_cap * 4 budget + cap_truncations under depth 2
  AC4 Soft observe path unchanged (hard only via linear_cross_closure_hard_enabled)
  AC5 schema-2612 + query keys + test/cmake/build.py gate; no design docs

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

    aud = _read("src/compiler/typed_mutation_audit.h")
    tci = _read("src/compiler/type_checker.ixx")
    tcpp = _read("src/compiler/type_checker_impl.cpp")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    q = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_linear_cross_closure_depth2_2612.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("linear_cross_closure_depth_cap", "AC1", aud)
    must("AURA_LINEAR_CROSS_CLOSURE_DEPTH", "AC1", aud)
    must("ac1_default_depth1", "AC1", test)
    must("default depth_cap == 1", "AC1", test)

    # AC2
    must("depth2_entries", "AC2", tci)
    must("depth2_escape_sites", "AC2", tci)
    must("depth_remaining", "AC2", tcpp)
    must("depth2_entries", "AC2", tcpp)
    must("linear_cross_closure_depth2_entries_total", "AC2", aud)
    must("linear_cross_closure_depth2_escape_total", "AC2", etc)
    must("ac2_depth2_discover_hard", "AC2", test)

    # AC3
    must("cone_cap * 4", "AC3", tcpp)
    must("cap_truncations", "AC3", tcpp)
    must("ac3_cone_trunc", "AC3", test)

    # AC4
    must("linear_cross_closure_hard_enabled", "AC4", etc)
    must("linear_cross_closure_observe_total", "AC4", etc)
    must("ac4_soft_observe", "AC4", test)

    # AC5
    must("schema-2612", "AC5", q)
    must("linear-cross-closure-depth-cap", "AC5", q)
    must("linear-cross-closure-depth2-entries-total", "AC5", q)
    must("test_linear_cross_closure_depth2_2612", "AC5", cmake)
    must("check_linear_cross_closure_depth2_2612", "AC5", build)
    must("cmd_linear_cross_closure_depth2_coverage", "AC5", build)
    must("ac5_schema_source", "AC5", test)
    for rel in (
        "docs/design/linear_cross_closure_depth2_2612.md",
        "docs/linear_cross_closure_depth2_2612.md",
        "design/2612.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2612 depth-2 cross-closure free-capture — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
