#!/usr/bin/env python3
"""Issue #2532: Write caps into Effect matrix"""

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

    src_core_capability_model_hh = _read("src/core/capability_model.hh")
    must("2532", "AC1", src_core_capability_model_hh)
    must('name == "workspace"', "AC1", src_core_capability_model_hh)
    must('name == "fiber"', "AC1", src_core_capability_model_hh)
    must("read-only observability", "AC2", src_core_capability_model_hh)
    CMakeLists_txt = _read("CMakeLists.txt")
    must("test_cap_write_effect_matrix_2532", "AC6", CMakeLists_txt)
    build_py = _read("build.py")
    must("check_2532", "AC6", build_py)

    if fails:
        print("check_2532: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_2532: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
