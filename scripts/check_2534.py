#!/usr/bin/env python3
"""Issue #2534: Security posture + correlated trail"""

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

    src_compiler_evaluator_primitives_security_cpp = _read("src/compiler/evaluator_primitives_security.cpp")
    must("query:security-posture", "AC1", src_compiler_evaluator_primitives_security_cpp)
    must("query:security-correlated-trail", "AC1", src_compiler_evaluator_primitives_security_cpp)
    must("schema-2534", "AC1", src_compiler_evaluator_primitives_security_cpp)
    CMakeLists_txt = _read("CMakeLists.txt")
    must("test_security_posture_trail_2534", "AC6", CMakeLists_txt)
    build_py = _read("build.py")
    must("check_2534", "AC6", build_py)

    if fails:
        print("check_2534: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_2534: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
