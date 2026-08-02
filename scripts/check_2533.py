#!/usr/bin/env python3
"""Issue #2533: Residual force safepoint"""

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

    src_serve_fiber_h = _read("src/serve/fiber.h")
    must("request_force_safepoint", "AC1", src_serve_fiber_h)
    src_serve_fiber_cpp = _read("src/serve/fiber.cpp")
    must("2533", "AC1", src_serve_fiber_cpp)
    src_compiler_evaluator_primitives_agent_cpp = _read("src/compiler/evaluator_primitives_agent.cpp")
    must("residual-force-safepoint-total", "AC4", src_compiler_evaluator_primitives_agent_cpp)
    must("schema-2533", "AC4", src_compiler_evaluator_primitives_agent_cpp)
    CMakeLists_txt = _read("CMakeLists.txt")
    must("test_residual_force_safepoint_2533", "AC6", CMakeLists_txt)
    build_py = _read("build.py")
    must("check_2533", "AC6", build_py)

    if fails:
        print("check_2533: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_2533: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
