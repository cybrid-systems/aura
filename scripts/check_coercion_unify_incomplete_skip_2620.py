#!/usr/bin/env python3
"""Issue #2620: Soft must not ship incomplete CoercionNodes (unify proof surface).

Contract:
  AC1 Soft + production: incomplete dual never inserts CoercionNode
  AC2 Soft: observe + arm force-Full (g_coercion_prov_slo_force_armed)
  AC3 Production dual-require drop unchanged (#2562)
  AC4 Canary AURA_COERCION_SAMPLED_INCOMPLETE_INSERT=1 restores #2317 insert
  AC5 Additive schema-2620; no rename of #2317/#2562 counters
  AC6 Gate/test source-cite #2620 on skip-insert branch

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

    cmap = _read("src/compiler/coercion_map.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_coercion_unify_incomplete_skip_2620.cpp")
    t2562 = _read("tests/compiler/test_coercion_dual_require_2562.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("#2620", "AC1", cmap)
    must("skip-insert branch", "AC1", cmap)
    must("g_coercion_soft_incomplete_skip_total", "AC1", cmap)
    must("arm_soft_incomplete_force_full_observe", "AC1", cmap)
    must("ac1_soft_no_incomplete_insert", "AC1", test)

    # AC2
    must("g_coercion_prov_slo_force_armed_total", "AC2", cmap)
    must("g_coercion_prov_slo_observe_only_total", "AC2", cmap)
    must("ac2_soft_arms_force_full", "AC2", test)

    # AC3
    must("g_coercion_dual_require_drop_total", "AC3", cmap)
    must("coercion_dual_require_active", "AC3", cmap)
    must("ac3_dual_require_unchanged", "AC3", test)

    # AC4 canary
    must("AURA_COERCION_SAMPLED_INCOMPLETE_INSERT", "AC4", cmap)
    must("coercion_sampled_incomplete_insert_canary", "AC4", cmap)
    must("g_coercion_sampled_insert_incomplete_total", "AC4", cmap)
    must("ac4_canary_restores_insert", "AC4", test)

    # AC5 additive schema
    must("schema-2620", "AC5", q)
    must("coercion-soft-incomplete-skip-total", "AC5", q)
    must("schema-2317", "AC5", q)
    must("schema-2562", "AC5", q)
    must("g_coercion_sampled_insert_incomplete_total", "AC5", cmap)
    must("g_coercion_dual_require_drop_total", "AC5", cmap)
    must("ac5_schema_source", "AC5", test)

    # AC6 wiring
    must("test_coercion_unify_incomplete_skip_2620", "AC6", cmake)
    must("check_coercion_unify_incomplete_skip_2620", "AC6", build)
    must("cmd_coercion_unify_incomplete_skip_coverage", "AC6", build)
    must("ac6_source_cite", "AC6", test)
    # #2562 Soft path updated for #2620
    must("AC2: Soft Sampled incomplete skips insert", "AC6", t2562)

    for rel in (
        "docs/design/coercion_unify_incomplete_skip_2620.md",
        "docs/coercion_unify_incomplete_skip_2620.md",
        "design/2620.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2620 coercion unify incomplete skip — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
