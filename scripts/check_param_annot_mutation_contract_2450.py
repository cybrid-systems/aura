#!/usr/bin/env python3
"""Issue #2450: param_annot_data_ single-threaded mutation contract.

Contract:
  AC1 FlatAST documents param_annot_data_ mutation contract (Issue #2450)
  AC2 add_lambda / set_lambda_params cite param_annot_data_.resize
  AC3 test + gate wiring

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

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_param_annot_mutation_contract_2450.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2450", "AC1", ast)
    must("param_annot_data_", "AC1", ast)
    must("single-threaded mutation", "AC1", ast)
    must("2450 AC1", "AC1", test)

    must("param_annot_data_.resize", "AC2", ast)
    must("add_lambda", "AC2", ast)
    must("set_lambda_params", "AC2", ast)
    must("2450 AC2", "AC2", test)

    must("2450 AC3", "AC3", test)
    must("check_param_annot_mutation_contract_2450", "gate", build)
    must("cmd_param_annot_mutation_contract_coverage", "gate", build)
    must("test_param_annot_mutation_contract_2450", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: param_annot_data_ mutation contract #2450 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
