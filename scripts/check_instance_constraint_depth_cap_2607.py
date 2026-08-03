#!/usr/bin/env python3
"""Issue #2607: minimal INSTANCE constraint + depth-capped instantiate.

Contract:
  AC1 INSTANCE kind + consistent_instance + worklist dispatch
  AC2 kInstanceDepthCap + depth_cap metric + TIMEOUT re-queue
  AC3 soft depth-cap not CONFLICT; escalate path retained
  AC4 schema-2607 + instance_* metrics on query surfaces
  AC5 test + cmake + build.py gate

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

    ix = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_instance_constraint_depth_cap_2607.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2607", "AC1", ix)
    must("INSTANCE", "AC1", ix)
    must("consistent_instance", "AC1", ix + impl)
    must("Constraint::INSTANCE", "AC1", impl)
    must("instance_unify_total", "AC1", met + fields + impl)

    # AC2
    must("kInstanceDepthCap", "AC2", ix + impl)
    must("instance_depth_cap_total", "AC2", met + fields + impl)
    must("depth_capped", "AC2", impl)

    # AC3 soft / escalate
    must("escalate_if_production", "AC3", impl)
    must("TIMEOUT", "AC3", impl)

    # AC4 query
    must("schema-2607", "AC4", q)
    must("issue-2607", "AC4", q)
    must("instance-goal-wired", "AC4", q)
    must("instance-unify-total", "AC4", q)
    must("instance-depth-cap-total", "AC4", q)
    must("type-repair-root-reason-instance", "AC4", q)
    must("Instance = 6", "AC4", ix)

    # AC5
    must("ac1_instance_solves_poly", "AC5", test)
    must("ac2_depth_cap_timeout", "AC5", test)
    must("test_instance_constraint_depth_cap_2607", "AC5", cmake)
    must("check_instance_constraint_depth_cap_2607", "AC5", build)
    must("cmd_instance_constraint_depth_cap_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2607 INSTANCE depth-cap constraint — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
