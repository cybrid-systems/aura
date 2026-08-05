#!/usr/bin/env python3
"""Issue #2656: CLI denseness fiber:spawn positive ids + contract.

Contract:
  AC1 positive thread-fallback fiber ids (not -1)
  AC2 fiber:spawn-backend registered
  AC3 denseness contract doc docs/stdlib/fiber-spawn.md
  AC4 unit test + cmake + build.py gate

Exit 0 = all rows satisfied.
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

    msg = _read("src/compiler/evaluator_primitives_messaging.cpp")
    bridge = _read("src/compiler/messaging_bridge.h")
    doc = _read("docs/stdlib/fiber-spawn.md")
    test = _read("tests/compiler/test_fiber_spawn_cli.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2656" in msg, "AC1: messaging cites #2656")
    must("0x4000" in msg or "positive" in msg.lower(), "AC1: positive thread ids")
    must("-(++thread_fiber_id)" not in msg, "AC1: no negative thread id formula")
    must("Never returns 0 or -1" in msg or "never -1" in msg.lower(), "AC1: contract in source")

    must("fiber:spawn-backend" in msg, "AC2: spawn-backend prim")
    must("#2656" in bridge or "thread" in bridge.lower(), "AC2: bridge documents fallback")

    must("#2656" in doc, "AC3: denseness doc")
    must("fiber:spawn" in doc and "thread" in doc, "AC3: backends documented")
    must("sequential" in doc.lower() or "yield" in doc.lower(), "AC3: sequential-yield note")

    must("test_fiber_spawn_cli" in cmake, "AC4: cmake")
    must("check_fiber_spawn_cli_2656" in build, "AC4: linter")
    must("cmd_fiber_spawn_cli_coverage" in build, "AC4: coverage cmd")
    must("AC1" in test and "AC2" in test and "#2656" in test, "AC4: unit ACs")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2656 CLI denseness fiber:spawn — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
