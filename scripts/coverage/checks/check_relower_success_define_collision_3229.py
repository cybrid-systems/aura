#!/usr/bin/env python3
"""Issue #3229: hashed-name 6-bit coverage needs a define-id side set.

#3136 ORs `1ULL << (fnv1a_64(name) & 63)` into last_success so residual
shrinks. Under large define sets that 6-bit slot collides: success(D)
clears residual for peer P. Production records a bounded define-id side
set; residual / remount / re-promote stay define-correct. Soft skip;
id==0 quiet. No new query key.

Contract:
  AC1 colliding peer residual not cleared by D's region bit
  AC2 Soft observe; quiet id==0
  AC3 #3136 hashed-name coverage retained
  AC4 Extend hot-update / re-promote / residual suites; linter; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/compiler/hot_update_registry.hh")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    svc = _read("src/compiler/service.ixx")
    dirty = _read("src/compiler/service_dirty.cpp")
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    t = _read("tests/compiler/test_hot_update_relower_success_coverage.cpp")
    force = _read("tests/compiler/test_force_jit_repromote.cpp")
    rec = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kRelowerSuccessDefineCollisionIssue", "AC1 stamp", hh)
    must("note_relower_success_define", "AC1 helper", hh)
    must("residual_force_for_define", "AC1 per-define residual", hh)
    must("note_relower_success_define", "AC1 store site", svc)
    must("note_relower_success_define", "AC1 cascade site", dirty)
    must("ac3229_1_collision_peer_stays_residual", "AC1 test", t)

    must("aura_production_defaults_active_probe() == 0", "AC2 Soft skip", hh)
    must("ac3229_2_soft_quiet", "AC2 test", t)

    must("note_relower_success_coverage(1ULL << (fnv1a_64(name) & 63))", "AC3 #3136", svc)
    must("last_reemit_success_region_mask_", "AC3 mask", hh)
    must("ac3229_3_no_regression_3136", "AC3 test", t)

    must("check_relower_success_define_collision_3229", "AC4 build.py", build)
    must("3229", "AC4 re-promote suite", force)
    must("3229", "AC4 residual suite", rec)
    must("Issue #3229", "AC4 remount", rt)
    must("relower_success_define_active_", "AC4 re-promote skip", cpp)
    if "schema-3229" in q:
        fails.append("AC4: new schema-3229 query key")
    if "g_3229_" in hh:
        fails.append("AC4: new g_3229_* counter")
    if (ROOT / "tests" / "issues" / "test_issue_3229.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3229.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3229.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3229.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3229-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3229 relower_success_define_collision:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3229 relower_success_define_collision: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
