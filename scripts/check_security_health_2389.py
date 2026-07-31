#!/usr/bin/env python3
"""Issue #2389: query:security-health single Agent score.

Contract:
  AC1 Score definition + weights in security_health.hh
  AC2 force_reason priority effect > isolation > fence > wal > wrap > ok
  AC3 Pure / additive (register_stats_impl; existing queries untouched)
  AC4 Keys health-bp / force-reason / schema-2389 / security-health-wired
  AC5 Tests + source-cite + catalog + gate

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

    hh = _read("src/compiler/security_health.hh")
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    test = _read("tests/compiler/test_security_health_2389.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 score definition
    must("health_bp", "AC1", hh)
    must("effect_deny_rate_bp", "AC1", hh)
    must("isolation_deny_rate_bp", "AC1", hh)
    must("fence_health_bp", "AC1", hh)
    must("wal_posture_bp", "AC1", hh)
    must("wrap_pressure_bp", "AC1", hh)
    must("compute_security_health", "AC1", hh)
    must("30u", "AC1", hh)  # weight for effect
    must("ac1_vacuous_healthy", "AC1", test)

    # AC2 force_reason
    must("effect-deny", "AC2", hh)
    must("isolation-deny", "AC2", hh)
    must("epoch-fence", "AC2", hh)
    must("wal-off", "AC2", hh)
    must("ring-wrap", "AC2", hh)
    must("ac2_effect_deny_and_priority", "AC2", test)

    # AC3 pure additive
    must("query:security-health", "AC3", sec)
    must("register_stats_impl", "AC3", sec)
    must("snapshot_capability_effect_stats", "AC3", sec)
    must("snapshot_tenant_isolation_stats", "AC3", sec)
    must("ac3_existing_queries_unchanged", "AC3", test)

    # AC4 keys
    must("health-bp", "AC4", sec)
    must("health-budget-bp", "AC4", sec)
    must("force-reason", "AC4", sec)
    must("schema-2389", "AC4", sec)
    must("security-health-wired", "AC4", sec)
    must("component-effect-deny-rate-bp", "AC4", sec)
    must("ac4_query_keys", "AC4", test)

    # AC5
    must("Issue #2389", "AC5", sec)
    must("query:security-health", "AC5", obs)
    must("test_security_health_2389", "AC5", cmake)
    must("check_security_health_2389", "AC5", build)
    must("cmd_security_health_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2389 security-health — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
