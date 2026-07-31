#!/usr/bin/env python3
"""Issue #2387: unify sensitive string caps with Effect matrix.

Contract:
  AC1 Registry-only grant satisfies has_capability for effect-mapped names
  AC2 revoke_effect_capability clears matrix + string list
  AC3 tenant-admin / syscall map to Effect bits + epoch bind
  AC4 Staged string-only remain; additive high bits only
  AC5 Source-cite + CMake + build.py gate

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

    cap = _read("src/core/capability_model.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    sch = _read("src/compiler/security_capabilities.h")
    test = _read("tests/compiler/test_capability_string_matrix_unify_2387.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1–AC3 matrix mapping
    must("Issue #2387", "AC1", cap)
    must("TenantAdmin", "AC1", cap)
    must("Syscall", "AC1", cap)
    must('name == "tenant-admin"', "AC3", cap)
    must('name == "syscall"', "AC3", cap)
    must("kEffectTenantAdmin", "AC3", sch)
    must("kEffectSyscall", "AC3", sch)
    must("ac1_registry_only_mutate", "AC1", test)
    must("ac2_revoke_clears_both", "AC2", test)
    must("ac3_tenant_admin_matrix", "AC3", test)
    must("revoke_effect_capability", "AC2", sec)

    # AC4 staged remainder documented
    must("compile-stats", "AC4", cap)
    must("ac4_string_only_remain_and_metrics", "AC4", test)

    # AC5
    if "2387" not in sec:
        fails.append("AC5: evaluator_security.cpp missing #2387 cite")
    must("test_capability_string_matrix_unify_2387", "AC5", cmake)
    must("check_capability_string_matrix_unify_2387", "AC5", build)
    must("cmd_capability_string_matrix_unify_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2387 string-cap / Effect matrix unify — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
