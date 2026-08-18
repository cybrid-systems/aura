#!/usr/bin/env python3
"""Issue #3119: production_defaults arm lock_order Hard.

Soft metric-only left inversions silent under multi-fiber. Production
Restricted/Strict now defaults to Hard (abort on inversion). Soft stays
via AURA_LOCK_ORDER_AUDIT=soft / sandbox=off. No new query keys.

Contract:
  AC1 apply_production_lock_order_default(false) → mode Hard (3)
  AC2 inversion under Hard fail-closed (abort path)
  AC3 sandbox=off / AUDIT=soft unchanged
  AC4 happy path same relaxed atomics; extend existing tests
  AC6 no test_issue_3119 / no docs/design; no schema-3119

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    lo = _read("src/compiler/lock_order_audit.h")
    sec = _read("src/compiler/security_defaults.hh")
    hard = _read("tests/compiler/test_lock_order_audit_hard.cpp")
    soft = _read("tests/compiler/test_lock_order_production_soft.cpp")
    build = _read("build.py")
    q = read_query_prims() + _read("src/compiler/evaluator_primitives_query_type_stats.cpp")

    must("Issue #3119", "AC1 header", lo)
    must("g_lock_order_mode.store(3", "AC1 production Hard store", lo)
    must("apply_production_lock_order_default", "AC1 helper", lo)
    must("3119 AC1", "AC1 test", hard)

    must("std::abort()", "AC2 abort on inversion", lo)
    must("lock_order_canary_enabled()", "AC2 Hard abort face", lo)
    must("3119 AC2", "AC2 test", hard)

    must("sandbox_off", "AC3 sandbox gate", lo)
    must("want_soft", "AC3 explicit Soft env", lo)
    must("3119 AC3", "AC3 sandbox=off test", hard)
    must("ac1_production_soft_inversion", "AC3 Soft still tested", soft)

    must("3119 AC4", "AC4 happy path", hard)
    must("check_lock_order_production_hard_3119", "AC4 build.py", build)
    must("#3119", "AC4 security_defaults", sec)

    if "schema-3119" in q:
        fails.append("AC6: new query key schema-3119 (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3119.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3119.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3119-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3119 production lock-order Hard — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
