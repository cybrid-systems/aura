#!/usr/bin/env python3
"""Issue #3482: steal/densify success must drop persist buffer with the face.

#3171 drops face bits + advances invalidate_gen. Residual: Occurrence
persist side-buffer and last-proof gauges survived, so rehydrate could
replay the pre-steal snapshot. Clear persist via existing #3170 helper.
Do not restamp green (#2938). Soft: observe only. No new query key.

Contract:
  AC1  production success helper calls clear_occurrence_persist_buffer
  AC2  steal + densify sites also Evaluator-clear persist
  AC3  #3225 seqlock + #3032 miss path do not gain persist clear
  AC4  Soft helper still early-returns before persist clear
  AC5  extend persist-rehydrate suite; linter AFTER #3171
  AC6  no invent / docs/design / schema-3482 / g_3482_*

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    hpos = tma.find("bool invalidate_fast_path_before_steal_densify_restamp()")
    hwin = tma[hpos : hpos + 1600] if hpos >= 0 else ""
    must("Issue #3482", "AC1 helper cite", hwin)
    must("clear_occurrence_persist_on_steal_densify_success_", "AC1 persist helper", hwin)
    must("production_defaults_active() || get_strategy() == AuditStrategy::Full", "AC1 hard gate", hwin)

    cpos = tma.find("Issue #3482: steal/densify success must drop the Occurrence persist")
    cwin = tma[cpos : cpos + 1400] if cpos >= 0 else ""
    must("clear_occurrence_persist_buffer(tc)", "AC1 reuse #3170", cwin)
    must("aura_typed_audit_current_commit_type_checker()", "AC1 commit TC", cwin)
    must_not("publish_last_proof_face(true", "AC1 no green restamp", cwin)

    steal = efm.find("UnifiedRestampSite::StealComplete || site == UnifiedRestampSite::Densify")
    swin = efm[steal : steal + 1800] if steal >= 0 else ""
    must("invalidate_fast_path_before_steal_densify_restamp", "AC2 steal helper", swin)
    must("aura_clear_occurrence_persist_buffer(this)", "AC2 steal persist clear", swin)
    must("Issue #3482", "AC2 steal cite", swin)

    dpos = mb.find("had_moving_densify = compact_r.moved_live_objects")
    dwin = mb[dpos : dpos + 1800] if dpos >= 0 else ""
    must("invalidate_fast_path_before_steal_densify_restamp", "AC2 densify helper", dwin)
    must("aura_clear_occurrence_persist_buffer(ev_)", "AC2 densify persist clear", dwin)
    must("Issue #3482", "AC2 densify cite", dwin)

    must("g_occurrence_persist_seq", "AC3 seqlock", tma)
    miss = tma.find("bool invalidate_fast_path_on_rehydrate_miss()")
    miss_win = tma[miss : miss + 1600] if miss >= 0 else ""
    must_not("clear_occurrence_persist_buffer", "AC3 miss no persist clear", miss_win)
    must("3482 AC3: miss path does not clear persist", "AC3 test", t)

    must("if (!hard)", "AC4 Soft early-out", hwin)
    must("3482 AC4: Soft returns false", "AC4 test", t)
    must("3482 AC1: persist buffer empty", "AC1 test", t)
    must("3482 AC2: rehydrate cannot restore pre-steal snapshot", "AC2 test", t)
    must("clear_occurrence_persist_buffer", "AC5 health cite", health)

    must("check_steal_densify_persist_clear_3482", "AC5 build.py", build)
    prev = build.find("check_linear_fast_path_clear_on_restamp_3171")
    ours = build.find("check_steal_densify_persist_clear_3482")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3171")
    must_not("schema-3482", "AC6 no schema-3482", tma)
    must_not("g_3482_", "AC6 no g_3482_*", tma)
    if _read("tests/compiler/test_issue_3482.cpp") or _read("tests/issues/test_issue_3482.cpp"):
        fails.append("AC6: test_issue_3482.cpp present (forbidden #81967)")
    if _read("docs/design/3482-steal-densify-persist.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3482 steal_densify_persist_clear:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3482 steal_densify_persist_clear: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
