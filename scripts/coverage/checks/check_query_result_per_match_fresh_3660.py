#!/usr/bin/env python3
"""Issue #3660: QueryResult freshness is per-match occupancy/wrap, not table epoch.

Production compared mutation_id_at_capture != current_mutation_epoch() for
every match, so an unrelated mutate:* invalidated the whole QueryResult.
Stamp also truncated the epoch into uint32.

Contract (one row per AC):
  AC1  production: unrelated mutate keeps unmodified match resolvable
  AC2  QueryEpoch finish_query_epoch still query-epoch-stale in-flight
  AC3  tenant/fiber/cow/schema-2 reserved gates stay
  AC4  stamp does not uint32-truncate current_mutation_epoch; no live
       mutation equality
  AC5  empty matches Fresh first; extends test_query_result_full_provenance;
       linter AFTER #3659; no invent; query:query-epoch-stats kept

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

    dec = _read("src/compiler/query_result_decode.hh")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    t = _read("tests/compiler/test_query_result_full_provenance.cpp")
    hh = _read("src/core/workspace_epoch.hh")
    build = _read("build.py")

    fn = dec.find("query_result_is_fresh_with_refs")
    win = dec[fn : fn + 3500] if fn >= 0 else ""

    must("Issue #3660", "AC1 cite", dec)
    must("is_live_node", "AC1 occupancy", win)
    must("test_ac3660_1_unrelated_mutate_keeps_unmodified_match", "AC1 test", t)
    if "uint64_t>(m.mutation_id_at_capture) != live_mutation" in win:
        fails.append("AC1: still whole-table mutation-epoch equality")

    must("finish_query_epoch", "AC2 QueryEpoch", qws)
    must("query-epoch-stale", "AC2 stale kind", qws)
    must("test_ac3660_2_query_epoch_in_flight", "AC2 test", t)

    must("InvalidTenant", "AC3 tenant", win)
    must("InvalidFiber", "AC3 fiber", win)
    must("InvalidCowLayer", "AC3 cow", win)
    must("kQueryResultMatchSchema2Prod", "AC3 reserved", win)
    must("test_ac3660_3_tenant_fiber_cow_reserved", "AC3 test", t)

    stamp = qws.find("stamp_query_result_full_provenance")
    swin = qws[stamp : stamp + 1800] if stamp >= 0 else ""
    must("Issue #3660", "AC4 stamp cite", swin)
    if "static_cast<std::uint32_t>(aura::core::current_mutation_epoch())" in swin:
        fails.append("AC4: stamp still truncates current_mutation_epoch to uint32")
    must("test_ac3660_4_no_uint32_epoch_stamp", "AC4 test", t)
    must("InvalidMutation = 5", "AC4 ABI retained", hh)

    empty_at = win.find("if (qr.match_count == 0)")
    gap_at = win.find("nested_authority_gap")
    if empty_at < 0 or (gap_at >= 0 and empty_at > gap_at):
        fails.append("AC5: empty matches must return Fresh before epoch/gap checks")
    must("test_ac3660_5_soft_empty_fresh_and_linter", "AC5 test", t)
    must("check_query_result_per_match_fresh_3660", "AC5 build.py", build)
    prev = build.find("check_boundary_safe_uses_snap_3659")
    ours = build.find("check_query_result_per_match_fresh_3660")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3659")
    must("query:query-epoch-stats", "AC5 epoch stats key", qws)
    if _read("tests/compiler/test_issue_3660.cpp"):
        fails.append("AC5: test_issue_3660.cpp present")
    if _read("docs/design/3660-query-result-per-match.md"):
        fails.append("AC5: docs/design/ exists")

    if fails:
        print("FAIL #3660 query_result_per_match_fresh:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3660 query_result_per_match_fresh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
