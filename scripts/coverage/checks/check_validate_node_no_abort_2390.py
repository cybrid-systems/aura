#!/usr/bin/env python3
"""Issue #2390: FlatAST::validate_node must not hard-abort on !is_valid.

Contract:
  AC1 validate_post_restore reports corrupt gen (no crash path via abort)
  AC2 fail_on_error=true → throw std::logic_error
  AC3 fail_on_error=false → return error string
  AC4 no std::abort() on !is_valid in validate_node
  AC5 tests + CMake + build.py gate

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

    impl = _read("src/core/ast_impl.cpp")
    test = _read("tests/core/test_validate_node_no_abort_2390.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1–AC3 behavior
    must("Issue #2390", "AC1", impl)
    must("node ID is not valid", "AC1", impl)
    must("throw std::logic_error", "AC2", impl)
    must("fail_on_error", "AC2", impl)
    must("ac1_post_restore_corrupt_reports", "AC1", test)
    must("ac2_validate_node_throws", "AC2", test)
    must("ac3_validate_node_returns_msg", "AC3", test)

    # AC4: abort must not sit on !is_valid path inside validate_node body.
    # Extract validate_node function body and ensure no abort there.
    m = re.search(
        r"std::string FlatAST::validate_node\(NodeId id, bool fail_on_error\) const \{",
        impl,
    )
    if not m:
        fails.append("AC4: validate_node definition not found")
    else:
        # Rough body: from match to next top-level function at column 0 starting with type.
        start = m.end()
        # Find matching close by scanning braces from the opening '{'
        brace = 1
        i = start
        while i < len(impl) and brace > 0:
            if impl[i] == "{":
                brace += 1
            elif impl[i] == "}":
                brace -= 1
            i += 1
        body = impl[start:i]
        if "std::abort()" in body or "abort()" in body:
            fails.append("AC4: validate_node still calls abort()")
        if "is not valid" not in body:
            fails.append("AC4: validate_node missing invalid-id error return")

    # AC5 registration
    must("test_validate_node_no_abort_2390", "AC5", cmake)
    must("check_validate_node_no_abort_2390", "AC5", build)
    must("cmd_validate_node_no_abort_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)
    must("Issue #2390", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2390 validate_node no hard-abort — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
