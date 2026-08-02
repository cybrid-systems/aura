#!/usr/bin/env python3
"""Issue #2536: Restricted hard-fiber optional policy contract."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def_ = _read("src/compiler/security_defaults.hh")
    cap = _read("src/core/capability_model.hh")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")
    test = _read("tests/compiler/test_hard_fiber_restricted_2536.cpp")

    must("2536", "AC5", def_)
    must("AURA_HARD_FIBER_ISOLATION", "AC2", def_)
    must("Restricted alone", "AC1", def_)
    must("2536", "AC5", cap)
    must("TenantScope", "AC5", cap)
    must("schema-2536", "AC5", sec)
    must("fiber-mismatch-total", "AC5", sec)
    must("fiber-hard-deny-total", "AC5", sec)
    must("test_hard_fiber_restricted_2536", "AC6", cmake)
    must("aura_issue_test_link_light(test_hard_fiber_restricted_2536)", "AC6", cmake)
    must("check_2536", "AC6", build)
    must("AC1", "AC6", test)

    if fails:
        print("check_2536: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_2536: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
