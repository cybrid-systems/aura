#!/usr/bin/env python3
"""Issue #3258: abort fence rejects concurrent lookup until force-dirty walk done.

lookup_define_v2 must not serve pre-abort IR / source_to_ir_map while
abort_force_in_progress_ is set or the entry's abort_force_generation
lags the live generation. gen==0 never-aborted path stays one acquire.

Contract:
  AC1  lookup rejects clean hit when in_progress or gen lag
  AC2  prepare_source_to_ir_map_for_partial_ also refuses
  AC3  gen bumped before in_progress (lag visible first)
  AC4  gen==0 short-circuit (Soft/Off zero extra)
  AC5  extend test_abort_ir_cache_fence_first; linter after #3257
  AC6  no docs/design/3258-*; no test_issue_3258.cpp

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

    svc = _read("src/compiler/service.ixx")
    t = _read("tests/compiler/test_abort_ir_cache_fence_first.cpp")
    build = _read("build.py")

    must("abort_force_rejects_clean_hit_", "AC1 helper", svc)
    lu = svc.find("int lookup_define_v2(")
    lu_win = svc[lu : lu + 2500] if lu >= 0 else ""
    must("abort_force_rejects_clean_hit_", "AC1 lookup", lu_win)
    must("Issue #3258", "AC1 lookup cite", lu_win)
    must("ac3258_1_concurrent_lookup_during_walk", "AC1 test", t)

    pr = svc.find("bool prepare_source_to_ir_map_for_partial_")
    pr_win = svc[pr : pr + 800] if pr >= 0 else ""
    must("abort_force_rejects_clean_hit_", "AC2 prepare", pr_win)
    must("ac3258_4_prepare_refuses_during_abort", "AC2 test", t)

    fence = svc.find("void begin_abort_ir_cache_force_fence()")
    fwin = svc[fence : fence + 700] if fence >= 0 else ""
    gen_pos = fwin.find("abort_force_generation_.fetch_add")
    ip_pos = fwin.find("abort_force_in_progress_.store(1")
    if gen_pos < 0 or ip_pos < 0 or gen_pos > ip_pos:
        fails.append("AC3: gen bump must precede in_progress store")
    must("Issue #3258", "AC3 fence cite", fwin)

    helper = svc.find("bool abort_force_rejects_clean_hit_")
    hwin = svc[helper : helper + 1500] if helper >= 0 else ""
    must("if (gen == 0)", "AC4 gen==0", hwin)
    must("return false", "AC4 never-aborted", hwin)
    must("ac3258_3_soft_gen0_zero_extra", "AC4 test", t)

    must("ac3258_5_source_and_linter", "AC5 test", t)
    must("check_abort_force_lookup_reject_3258", "AC5 build.py", build)
    prev = build.find("check_deferred_hybrid_rearm_last_look_3257")
    ours = build.find("check_abort_force_lookup_reject_3258")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3257")

    if (ROOT / "tests" / "issues" / "test_issue_3258.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3258.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3258.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3258.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3258-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3258 abort_force_lookup_reject:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3258 abort_force_lookup_reject: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
