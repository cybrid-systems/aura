#!/usr/bin/env python3
"""Issue #2510: transactional LayoutStamp + provenance restamp on steal-complete.

Contract:
  AC1: on_steal_complete sole restamp entry (source-cite + worker call)
  AC2: LayoutStamp mismatch under Hard → hard-fail (cancel+Done)
  AC3: Soft metric-only; production Soft ignored; hard_fail observable
  AC4: match → restamp; inject drift → mismatch (no silent green)
  AC5: stress N + unit test + gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    wc = _read("src/serve/worker.cpp")
    met = _read("src/compiler/observability_metrics.h")
    eixx = _read("src/compiler/evaluator.ixx")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    jit = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/serve/test_steal_complete_restamp_txn_2510.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 sole restamp entry
    must("aura_evaluator_on_steal_complete", "AC1", fm)
    must("Issue #2510", "AC1", fm)
    must("refresh_stale_frames_after_steal", "AC1", fm)
    must("steal_complete_restamp_total", "AC1", fm)
    must("call_steal_complete(stolen)", "AC1", wc)
    must("is_cancel_requested", "AC1", wc)
    must("AC1", "AC1", test)

    # AC2 hard-fail on mismatch
    must("steal_complete_layout_hard_fail_total", "AC2", fm)
    must("bump_steal_snapshot_hard_fail", "AC2", fm)
    must("request_cancel", "AC2", fm)
    must("FiberState::Done", "AC2", fm)
    must("is_steal_snapshot_hard_mode", "AC2", fm)
    must("AC2", "AC2", test)

    # AC3 Soft / production
    must("is_steal_snapshot_hard_mode", "AC3", fm)
    must("AC3", "AC3", test)
    must("steal_snapshot_hard_fail", "AC3", test)
    must("steal_snapshot_soft_production_locked", "AC3", _read("src/serve/fiber.cpp"))

    # AC4 restamp metrics + dual-check retained
    must("steal_complete_restamp_total", "AC4", met)
    must("steal_complete_layout_hard_fail_total", "AC4", met)
    must("get_steal_complete_restamp_total", "AC4", eixx)
    must("get_steal_complete_restamp_total", "AC4", emb)
    must("layout_stamp_steal_mismatch_total", "AC4", fm)
    must("AC4", "AC4", test)

    # AC5 stress + wiring + schema
    must("schema-2510", "AC5", q)
    must("steal-complete-restamp-total", "AC5", q)
    must("steal-complete-layout-hard-fail-total", "AC5", q)
    must("schema-2510", "AC5", jit)
    must("test_steal_complete_restamp_txn_2510", "AC5", cmake)
    must("check_steal_complete_restamp_txn_2510", "AC5", build)
    must("cmd_steal_complete_restamp_txn_coverage", "AC5", build)
    must("AC5", "AC5", test)
    must("schema-2351", "AC5", q)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2510 steal-complete restamp transaction — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
