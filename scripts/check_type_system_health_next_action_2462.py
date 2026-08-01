#!/usr/bin/env python3
"""Issue #2462: type-system-health next-action + repair_nodes.

Contract:
  AC1 healthy → ok
  AC2 truncated/production incomplete → full-solve/rollback
  AC3 TIMEOUT+repair → expand-dirty
  AC4 castop over budget → annotate-dynamic
  AC5 pure decide + schema-2462 + #2350 intact + gate wiring

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

    hh = _read("src/compiler/type_system_health.hh")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_type_system_health_next_action_2462.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("decide_type_system_next_action", "AC1", hh)
    must("TypeSystemNextAction", "AC1", hh)
    must('"ok"', "AC1", hh)
    must("AC1: healthy empty", "AC1", test)

    must("full-solve", "AC2", hh)
    must("rollback", "AC2", hh)
    must("truncated_reverify", "AC2", hh)
    must("AC2: truncated", "AC2", test)

    must("expand-dirty", "AC3", hh)
    must("repair_nodes_count", "AC3", hh)
    must("AC3: expand-dirty", "AC3", test)

    must("annotate-dynamic", "AC4", hh)
    must("castop_over_budget", "AC4", hh)
    must("AC4: annotate-dynamic", "AC4", test)

    must("schema-2462", "AC5", q)
    must("next-action", "AC5", q)
    must("repair-nodes-count", "AC5", q)
    must("type-system-health-next-action-wired", "AC5", q)
    must("schema-2350", "AC5", q)
    must("Issue #2462", "AC5", hh)
    must("Issue #2462", "AC5", q)
    must("test_type_system_health_next_action_2462", "gate", cmake)
    must("check_type_system_health_next_action_2462", "gate", build)
    must("cmd_type_system_health_next_action_coverage", "gate", build)
    must("AC5: pure", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: type-system-health next-action #2462 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
