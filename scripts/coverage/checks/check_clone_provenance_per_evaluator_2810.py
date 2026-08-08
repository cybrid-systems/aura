#!/usr/bin/env python3
"""Issue #2810: clone_macro_body provenance repin dual-writes per-CompilerMetrics.

Contract (one row per AC):
  AC1 clone_macro_body resolves Evaluator; no nullptr-only repin call; cites #2810
  AC2 bridge dual-write + return 2/1 contract; resolve/bump helpers
  AC3 tests/compiler/test_clone_provenance_per_evaluator.cpp
  AC4 this linter wired; no docs/design/2810-*; no test_issue_2810.cpp

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
            fails.append(f"{label}: forbidden {n!r} present")

    me = _read("src/compiler/macro_expansion.cpp")
    ixx = _read("src/compiler/macro_expansion.ixx")
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    bridge_h = _read("src/compiler/aura_jit_bridge.h")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_clone_provenance_per_evaluator.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 — clone path
    must("Issue #2810", "AC1", me)
    must("aura_evaluator_resolve_current_for_macro", "AC1", me)
    must("g_clone_macro_provenance_per_evaluator_total", "AC1", me)
    must("g_clone_macro_provenance_per_evaluator_total", "AC1", ixx)
    # Forbidden: the pre-#2810 nullptr-only call site.
    must_not("aura_macro_provenance_repin_on_steal(nullptr", "AC1", me)

    # AC2 — bridge + trampolines
    must("Issue #2810", "AC2", bridge)
    must("aura_evaluator_bump_macro_provenance_repin_on_steal", "AC2", bridge)
    must("aura_evaluator_resolve_current_for_macro", "AC2", fiber)
    must("bump_macro_provenance_repin_on_steal_total", "AC2", fiber)
    must("aura_evaluator_bump_macro_provenance_repin_on_steal", "AC2", fiber)
    must("Issue #2810", "AC2", bridge_h)
    must("aura_macro_provenance_repin_on_steal", "AC2", bridge_h)
    must("aura_clone_macro_provenance_per_evaluator_total_v_read", "AC2", bridge_h)
    must("aura_evaluator_resolve_current_for_macro", "AC2", stub)
    must("aura_evaluator_bump_macro_provenance_repin_on_steal", "AC2", stub)

    # AC3 — test
    must("ac2810", "AC3", test)
    must("2810", "AC3", test)
    must("macro_provenance_repin_on_steal_total", "AC3", test)
    must("g_clone_macro_provenance_per_evaluator_total", "AC3", test)
    must("clone_macro_body", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_clone_provenance_per_evaluator.cpp").is_file():
        fails.append("AC3: missing test_clone_provenance_per_evaluator.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2810.cpp").is_file():
        fails.append("AC3: test_issue_2810.cpp present (forbidden per #81967)")
    must("test_clone_provenance_per_evaluator", "AC3", cmake)

    # AC4
    must("check_clone_provenance_per_evaluator_2810", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2810-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2810 clone provenance per-Evaluator dual-write — metrics visible")
    return 0


if __name__ == "__main__":
    sys.exit(main())
