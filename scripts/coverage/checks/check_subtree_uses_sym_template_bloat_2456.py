#!/usr/bin/env python3
"""Issue #2456: hoist find_first_node_with instantiation to single TU.

Contract:
  AC1 subtree_uses_sym declared in ast.ixx, defined in ast_impl.cpp
  AC2 named functors VariableUsesSymPred / DefineSymPred (not per-call lambdas)
  AC3 find_define_by_name also out-of-line; test + gate wiring

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


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ast = _read("src/core/ast.ixx")
    impl = _read("src/core/ast_impl.cpp")
    test = _read("tests/core/test_subtree_uses_sym_template_bloat.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2456", "AC1", ast)
    must("Issue #2456", "AC1", impl)
    # Declaration (not full lambda body) in interface unit
    must("bool subtree_uses_sym(aura::ast::NodeId root, SymId sym) const;", "AC1", ast)
    must("FlatAST::subtree_uses_sym", "AC1", impl)

    must("VariableUsesSymPred", "AC2", impl)
    must("DefineSymPred", "AC2", impl)
    # No interface-unit lambda capture for these two helpers
    if re.search(
        r"subtree_uses_sym\([^{]*\{[^}]*find_first_node_with[^{]*\[[^\]]*this",
        ast,
        re.DOTALL,
    ):
        fails.append("AC2: subtree_uses_sym still has interface-unit lambda body")
    if re.search(
        r"find_define_by_name\([^{]*\{[^}]*find_first_node_with[^{]*\[[^\]]*this",
        ast,
        re.DOTALL,
    ):
        fails.append("AC2: find_define_by_name still has interface-unit lambda body")

    must("FlatAST::find_define_by_name", "AC3", impl)
    must("2456 AC3", "AC3", test)
    must("check_subtree_uses_sym_template_bloat_2456", "gate", build)
    must("cmd_subtree_uses_sym_template_bloat_coverage", "gate", build)
    must("test_subtree_uses_sym_template_bloat", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: subtree_uses_sym single-TU template hoist #2456 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
