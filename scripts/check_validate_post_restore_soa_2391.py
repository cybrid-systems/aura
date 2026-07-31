#!/usr/bin/env python3
"""Issue #2391: validate_post_restore SoA column size cross-check.

Contract:
  AC1 SoA size mismatch (sym_id_ / int_val_ / …) recorded as violation
  AC2 Happy-path healthy FlatAST remains zero SoA violations
  AC3 Source-cite + tests + CMake + build.py gate
  AC4 No hard-abort on size check path

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

    impl = _read("src/core/ast_impl.cpp")
    ixx = _read("src/core/ast.ixx")
    test = _read("tests/core/test_validate_post_restore_soa_2391.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 cross-check present
    must("Issue #2391", "AC1", impl)
    must("SoA column", "AC1", impl)
    must("int_val_", "AC1", impl)
    must("sym_id_", "AC1", impl)
    must("node_gen_", "AC1", impl)
    must("type_id_", "AC1", impl)
    must("children_", "AC1", impl)
    must("parent_", "AC1", impl)
    must("ac1_soa_sym_id_drift", "AC1", test)

    # AC2 happy path
    must("ac2_happy_path", "AC2", test)

    # AC3 registration
    must("2391", "AC3", ixx)
    must("test_validate_post_restore_soa_2391", "AC3", cmake)
    must("check_validate_post_restore_soa_2391", "AC3", build)
    must("cmd_validate_post_restore_soa_coverage", "AC3", build)
    must("ac3_source_and_gate", "AC3", test)
    must("Issue #2391", "AC3", test)

    # AC4: size check must not abort
    # Ensure abort not introduced in validate_post_restore body for size path.
    if "SoA column" in impl and "std::abort()" in impl:
        # Only fail if abort appears between validate_post_restore and next function.
        start = impl.find("PostRestoreReport FlatAST::validate_post_restore")
        if start >= 0:
            body = impl[start : start + 4000]
            if "std::abort()" in body:
                fails.append("AC4: validate_post_restore still contains std::abort()")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2391 validate_post_restore SoA size cross-check — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
