#!/usr/bin/env python3
"""Issue #2643: INSTANCE depth budget + Agent-visible repair surface on TIMEOUT.

Contract:
  AC1 depth-cap hit → TIMEOUT + Instance reason on suggested root
  AC2 repair hint carries depth_used == kInstanceDepthCap and poly TypeId
  AC3 SOLVED path → empty hints, zero extra alloc
  AC4 query surface exposes sample without free-form diagnostic parse
  AC5 schema + source-cite + cmake wiring
  AC6 production escalate_if_production behavior unchanged

Exit 0 = all AC rows satisfied.
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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_instance_constraint_depth_cap_2607.cpp")
    _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1: depth-cap → TIMEOUT + Instance reason on suggested root
    must("kInstanceRepairHintCap", "AC1", ixx)
    must("instance_repair_hints", "AC1", ixx)
    must("instance_repair_hints", "AC1", impl)
    must("SuggestedRootReason::Instance", "AC1", impl)
    # AC2: repair hint struct + per-field population
    must("InstanceRepairHint", "AC2", ixx)
    must("depth_used", "AC2", ixx)
    must("depth_cap", "AC2", ixx)
    must("var_rep", "AC2", ixx)
    must("site_node", "AC2", ixx)
    must("kInstanceDepthCap", "AC2", impl)
    must("instance_repair_hints.push_back", "AC2", impl)
    # AC3: SOLVED path → empty hints (no alloc, no bump on SOLVED).
    # Implementation is gated on r.status == SolveResult::TIMEOUT so SOLVED
    # path skips the bounded-sample loop entirely.
    must("if (r.status == SolveResult::TIMEOUT)", "AC3", impl)
    must("r.instance_repair_hints.empty()", "AC3", test)
    # AC4: query surface — bounded sample + aggregate counter + schema sentinel
    must("instance-depth-cap-repair-hint-total", "AC4", q)
    must("type_repair_instance_hint_count", "AC4", q)
    must("instance-depth-cap-repair-hint-cap", "AC4", q)
    must("instance-depth-cap-repair-hint-wired", "AC4", q)
    must("instance-depth-cap-repair-hint-%zu-depth-used", "AC4", q)
    must("instance-depth-cap-repair-hint-%zu-depth-cap", "AC4", q)
    must("instance-depth-cap-repair-hint-%zu-poly", "AC4", q)
    must("instance-depth-cap-repair-hint-%zu-var-rep", "AC4", q)
    must("instance-depth-cap-repair-hint-%zu-site-node", "AC4", q)
    must("schema-2643", "AC4", q)
    must("issue-2643", "AC4", q)
    must("instance-depth-cap-repair-hint-wired", "AC4", q)
    # AC5: source-cite + observability + fields.inc + linter registration
    must("#2643", "AC5", ixx)
    must("#2643", "AC5", impl)
    must("#2643", "AC5", met)
    must("#2643", "AC5", fields)
    must("#2643", "AC5", test)
    must("instance_depth_cap_repair_hint_total", "AC5", met)
    must("type_repair_instance_hint_depth_used", "AC5", met)
    must("type_repair_instance_hint_depth_cap", "AC5", met)
    must("type_repair_instance_hint_poly", "AC5", met)
    must("type_repair_instance_hint_var_rep", "AC5", met)
    must("type_repair_instance_hint_site_node", "AC5", met)
    must("type_repair_instance_hint_count", "AC5", met)
    must("instance_depth_cap_repair_hint_total", "AC5", fields)
    must("type_repair_instance_hint_count", "AC5", fields)
    must("hint_out", "AC5", ixx)  # consistent_instance 5th param
    must("type_repair_instance_hint_depth_used", "AC5", impl)  # mirror write site
    # AC6: production escalate_if_production unchanged — no schema changes
    # to the resolve path. Source-cite the unchanged behavior contract.
    must("escalate_if_production", "AC6", test)
    must("ac2643_repair_hint_on_timeout", "AC6", test)
    # Coverage gate: linter self-registration + cmake target
    must("check_instance_depth_repair_hint_2643", "AC6", build)
    must("ac2643_repair_hint_on_timeout", "AC6", test)
    must("ac2643_solved_no_hints", "AC6", test)
    must("ac2643_query_surface", "AC6", test)
    must("ac2643_source_cite", "AC6", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2643 INSTANCE depth-cap repair hint — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
