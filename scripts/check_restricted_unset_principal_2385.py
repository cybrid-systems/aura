#!/usr/bin/env python3
"""Issue #2385: Restricted sandbox denies side-effects when principal unset.

Contract:
  AC1 Restricted + tenant=0 + side-effect → deny
  AC2 Restricted + set principal → allow
  AC3 Strict + tenant=0 + side-effect → deny
  AC4 Off + tenant=0 → permissive
  AC5 Restricted + effects=0 → allow (query-only)
  AC6 Source-cite + CMake + build.py gate

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

    iso = _read("src/core/workspace_isolation.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    test = _read("tests/core/test_restricted_unset_principal_2385.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 policy in check_boundary_ex
    must("Issue #2385", "AC1", iso)
    must("sandbox_restricted", "AC1", iso)
    must("need_principal", "AC1", iso)
    must("required_effects != 0", "AC1", iso)
    must("ac1_restricted_unset_denies_side_effect", "AC1", test)
    must("isolation-deny:unset-principal", "AC1", sec)
    must("isolation-deny:unset-principal", "AC1", test)

    # AC2–AC5 tests
    must("ac2_restricted_with_principal_allows", "AC2", test)
    must("ac3_strict_unset_denies", "AC3", test)
    must("ac4_off_unset_permissive", "AC4", test)
    must("ac5_restricted_pure_read_allows", "AC5", test)

    # AC6 registration
    must("test_restricted_unset_principal_2385", "AC6", cmake)
    must("check_restricted_unset_principal_2385", "AC6", build)
    must("cmd_restricted_unset_principal_coverage", "AC6", build)
    must("ac6_source_and_gate", "AC6", test)
    must("Issue #2385", "AC6", test)
    must("Issue #2385", "AC6", sec)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2385 Restricted unset principal deny — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
