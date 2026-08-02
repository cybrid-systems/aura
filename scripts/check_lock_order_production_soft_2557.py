#!/usr/bin/env python3
"""Issue #2557: production soft lock-order audit coverage.

Contract:
  AC1 apply_production_lock_order_default + production soft flag
  AC2 sandbox=off → OFF path
  AC3 canary precedence documented / hard mode
  AC4 security_defaults wires soft default
  AC5 test + cmake + build.py + query schema-2557

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

    lo = _read("src/compiler/lock_order_audit.h")
    sec = _read("src/compiler/security_defaults.hh")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_lock_order_production_soft_2557.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2557", "AC1", lo)
    must("apply_production_lock_order_default", "AC1", lo)
    must("g_lock_order_production_soft_default", "AC1", lo)
    must("lock_order_production_soft_active", "AC1", lo)
    must("ac1_production_soft_inversion", "AC1", test)

    # AC2
    must("sandbox_off", "AC2", lo)
    must("ac2_sandbox_off", "AC2", test)

    # AC3
    must("AURA_LOCK_ORDER_CANARY", "AC3", lo)
    must("ac3_canary_precedence", "AC3", test)

    # AC4 wire
    must("apply_production_lock_order_default", "AC4", sec)
    must("#2557", "AC4", sec)
    must("lock_order_audit.h", "AC4", sec)

    # AC5 gate + query
    must("query:lock-order-audit-stats", "AC5", q)
    must("schema-2557", "AC5", q)
    must("production-soft-active", "AC5", q)
    must("test_lock_order_production_soft_2557", "AC5", cmake)
    must("check_lock_order_production_soft_2557", "AC5", build)
    must("cmd_lock_order_production_soft_coverage", "AC5", build)
    must("ac5_source_schema", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2557 production soft lock-order audit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
