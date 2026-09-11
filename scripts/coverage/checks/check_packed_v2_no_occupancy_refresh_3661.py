#!/usr/bin/env python3
"""Issue #3661: production packed v2 gen mismatch is stale-ref, not occupancy remake.

resolve_mutate_node_arg / resolve_query_node_arg still called
ensure_valid_or_refresh(ref, auto_refresh=true). refresh_if_stale remade
the current occupant's gen when the slot was live and wrap/cow/tenant
matched. Strict StaleRefPolicy only ran after refresh failed.

Contract (one row per AC):
  AC1  production packed resolve uses auto_refresh=false (refresh = !prod)
  AC2  matching-gen v2 still goes through ensure_valid_or_refresh
  AC3  Soft auto_refresh=true historical path kept (bare-int / !prod)
  AC4  Strict stale-ref still on the ensure-fail path
  AC5  extend as-stable-ref / tenant / hygiene suites; linter AFTER #3660;
       no invent; no docs/design; wrap/#3395/#3396 non-regress

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    stab = _read("src/core/ast_stability.cpp")
    t = _read("tests/serve/test_stable_ref_provenance_fiber_cow.cpp")
    hyg = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    ten = _read("tests/compiler/test_stable_ref_tenant_mandate.cpp")
    build = _read("build.py")

    rmut = mut.find("auto resolve_mutate_node_arg")
    mwin = mut[rmut : rmut + 9000] if rmut >= 0 else ""
    packed = mwin.find("if (auto packed = unpack_stable_ref_arg")
    pint = mwin.find("if (is_int(arg))")
    pwin = mwin[packed:pint] if packed >= 0 and pint > packed else ""

    must("Issue #3661", "AC1 mutate cite", pwin)
    must("auto_refresh=*/refresh", "AC1 mutate refresh flag", pwin)
    must("production_defaults_active()", "AC1 mutate production gate", pwin)
    if "auto_refresh=*/true" in pwin:
        fails.append("AC1: packed mutate resolve still auto_refresh=true")

    rq = qws.find("auto resolve_query_node_arg")
    qwin = qws[rq : rq + 9000] if rq >= 0 else ""
    must("Issue #3661", "AC1 query cite", qwin)
    must("auto_refresh=*/refresh", "AC1 query refresh flag", qwin)
    must("production_defaults_active()", "AC1 query production gate", qwin)
    must("test_ac3661_1_expired_gen_stale_ref", "AC1 test", t)

    must("ensure_valid_or_refresh", "AC2 mutate ensure", pwin)
    must("ensure_valid_or_refresh", "AC2 query ensure", qwin)
    must("test_ac3661_2_matching_gen_succeeds", "AC2 test", t)

    # Soft bare-int path in mutate still auto_refresh=true.
    iwin = mwin[pint : pint + 4000] if pint >= 0 else ""
    must("auto_refresh=*/true", "AC3 Soft mutate bare-int", iwin)
    must("test_ac3661_3_soft_auto_refresh", "AC3 test", t)

    must("StaleRefPolicy::Strict", "AC4 Strict", mwin)
    must("stable-ref is stale (Strict policy blocked)", "AC4 Strict msg", mwin)
    must("test_ac3661_4_strict_not_bypassed", "AC4 test", t)

    must("test_ac3661_5_suites_and_linter", "AC5 test", t)
    must("ac3661_hygiene_source_cite", "AC5 hygiene", hyg)
    must("3661", "AC5 tenant", ten)
    must("check_packed_v2_no_occupancy_refresh_3661", "AC5 build.py", build)
    prev = build.find("check_query_result_per_match_fresh_3660")
    ours = build.find("check_packed_v2_no_occupancy_refresh_3661")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3660")
    must("Issue #3661", "AC5 refresh_if_stale cite", stab)
    must("raw node-id rejected under production", "AC5 #3395", mwin)
    must("walk_v2", "AC5 #3396", mut)
    must("wrap_epoch != 0 && wrap_epoch != ast.wrap_epoch()", "AC5 #2393 wrap", stab)
    if _read("tests/compiler/test_issue_3661.cpp"):
        fails.append("AC5: test_issue_3661.cpp present")
    if _read("docs/design/3661-packed-v2-no-occupancy-refresh.md"):
        fails.append("AC5: docs/design/ exists")

    if fails:
        print("FAIL #3661 packed_v2_no_occupancy_refresh:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3661 packed_v2_no_occupancy_refresh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
