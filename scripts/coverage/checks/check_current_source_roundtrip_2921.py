#!/usr/bin/env python3
"""Issue #2921: roundtrip + regression matrix for current-source / snapshot.

AC:
  1. C++ unit test with kRoundtripNoMutate table + dual-workspace / mutate cases
  2. Suite smoke present
  3. CMake + build.py wiring
  4. Doc pointer for extending the table
  5. Cross-cites #2918/#2919/#2920
"""

from __future__ import annotations

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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    unit = _read("tests/compiler/test_current_source_roundtrip.cpp")
    must("2921" in unit, "AC1: unit cites #2921")
    must("kRoundtripNoMutate" in unit, "AC1: table")
    must("2918" in unit and "2919" in unit and "2920" in unit, "AC5: cross-cites")
    must("mutate:rebind" in unit, "AC1: mutate case")
    must("ast:snapshot" in unit, "AC1: snapshot case")
    must("Extending the table" in unit or "Extend here" in unit, "AC4: extend docs in unit")

    suite = _read("tests/suite/current_source_roundtrip_2921.aura")
    must("2921" in suite and "current-source :workspace" in suite, "AC2: suite")

    cmake = _read("CMakeLists.txt")
    must("test_current_source_roundtrip" in cmake, "AC3: cmake")

    build = _read("build.py")
    must(
        "current-source-roundtrip-2921" in build or "current_source_roundtrip_2921" in build,
        "AC3: build.py",
    )

    doc = _read("docs/stdlib/current-source-roundtrip.md")
    must("2921" in doc and "kRoundtripNoMutate" in doc, "AC4: extend doc")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2921 current-source roundtrip matrix — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
