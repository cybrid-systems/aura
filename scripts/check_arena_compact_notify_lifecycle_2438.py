#!/usr/bin/env python3
"""Issue #2438: arena compact notify_* TOCTOU / lifecycle drain.

Contract:
  AC1 documented invariant + clear_arena_compact_notify_hooks
  AC2 CompilerService dtor clear before g_current null
  AC3 in_flight around notify_*
  AC4 test + gate

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

    gh = _read("src/core/gc_hooks.h")
    svc = _read("src/compiler/service.ixx")
    test = _read("tests/core/test_arena_compact_notify_lifecycle_2438.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2438", "AC1", gh)
    must("clear_arena_compact_notify_hooks", "AC1", gh)
    must("g_arena_compact_hook_in_flight", "AC1", gh)
    must("2438 AC1", "AC1", test)

    must("clear_arena_compact_notify_hooks", "AC2", svc)
    must("Issue #2438", "AC2", svc)
    must("2438 AC2", "AC2", test)

    must("g_arena_compact_hook_in_flight.fetch_add", "AC3", gh)
    must("notify_auto_compact_trigger", "AC3", gh)
    must("2438 AC3", "AC3", test)

    must("2438 AC4", "AC4", test)
    must("check_arena_compact_notify_lifecycle_2438", "gate", build)
    must("cmd_arena_compact_notify_lifecycle_coverage", "gate", build)
    must("test_arena_compact_notify_lifecycle_2438", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: arena compact notify lifecycle #2438 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
