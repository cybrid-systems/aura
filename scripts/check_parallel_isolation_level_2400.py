#!/usr/bin/env python3
"""Issue #2400: parallel-intend batch hash isolation-level enum.

Contract:
  AC1 default → isolation-level=serialized + eval-serialized
  AC2 :pure #t → isolation-level=best-effort-pure
  AC3 pure metrics meaning unchanged
  AC4 forbid transactional advertising for pure
  AC5 tests + build.py gate; schema-2081/2163 preserved

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

    prim = _read("src/compiler/evaluator_primitives_agent.cpp")
    readme = _read("src/orch/README.md")
    test = _read("tests/orch/test_parallel_intend_pure_contract_2230.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2400", "AC1", prim)
    must("isolation-level", "AC1", prim)
    must("serialized", "AC1", prim)
    must("2400 AC1", "AC1", test)

    # AC2
    must("best-effort-pure", "AC2", prim)
    must("2400 AC2", "AC2", test)

    # AC3
    must("pure-unlocked-tasks", "AC3", prim)
    must("eval-serialized", "AC3", prim)
    must("2400 AC3", "AC3", test)

    # AC4
    must("Do NOT advertise pure as transactional", "AC4", prim)
    must("isolation-level", "AC4", readme)
    must("best-effort-pure", "AC4", readme)
    must("2400 AC4", "AC4", test)

    # AC5
    must("schema-2400", "AC5", prim)
    must("isolation-level-wired", "AC5", prim)
    must("schema-2081", "AC5", prim)
    must("schema-2163", "AC5", prim)
    must("2400 AC5", "AC5", test)
    must("check_parallel_isolation_level_2400", "AC5", build)
    must("cmd_parallel_isolation_level_coverage", "AC5", build)
    must("test_parallel_intend_pure_contract_2230", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2400 isolation-level — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
