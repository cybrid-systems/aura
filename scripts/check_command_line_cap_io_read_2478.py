#!/usr/bin/env python3
"""Issue #2478: command-line requires kCapIoRead via deny_io.

Contract:
  AC1 deny_io(kCapIoRead) in command-line body
  AC2 #2478 cite + denial message
  AC3 sibling pattern (deny_io captured)
  AC4 test present
  AC5 gate wiring

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

    src = _read("src/compiler/evaluator_primitives_file.cpp")
    test = _read("tests/compiler/test_command_line_cap_io_read_2478.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    idx = src.find('add("command-line"')
    # Include preceding #2478 comment block (~200 chars before add).
    start = max(0, idx - 250) if idx >= 0 else 0
    body = src[start : idx + 900] if idx >= 0 else ""

    must("Issue #2478", "AC1", src)
    must("deny_io", "AC1", body)
    must("kCapIoRead", "AC1", body)
    must("io-read required", "AC1", body)

    must("Issue #2478", "AC2", body)
    must("2478 AC1", "AC2", test)
    must("capability denied", "AC2", test.lower())

    must("deny_io", "AC3", body)
    # Capture deny_io in lambda (not bare [&ev])
    must("deny_io]", "AC3", body)  # [&ev, deny_io]

    must("test_command_line_cap_io_read_2478", "AC4", test)
    must("kCapIoRead", "AC4", test)

    must("check_command_line_cap_io_read_2478", "gate", build)
    must("cmd_command_line_cap_io_read_coverage", "gate", build)
    must("test_command_line_cap_io_read_2478", "gate", cmake)
    must("2478 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: command-line kCapIoRead #2478 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
