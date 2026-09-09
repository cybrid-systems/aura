#!/usr/bin/env python3
# scripts/check_dual_graph_parity_cone_3615.py -- Issue #3615 source-cite gate.
#
# AC1: cone-wide dual-graph parity check + Soft-erased hole detection live
#      in fail_closed_soft_dual_graph_parity_before_partial_cone_, called
#      from relower_dirty_defines_from_workspace (replaces the front-only
#      #3486 consult).
# AC2: Soft / Off zero-cost (production/Full gate only) — Soft single-fiber
#      path skips the cone helper entirely.
# AC3: No new query key, no docs/design/*3615-*, no test_issue_3615.cpp.
# AC4: ACs in tests/compiler/test_dep_graph_hybrid_cascade.cpp
#      (ac3615_1_cone_wide_non_front_fork, ac3615_2_soft_erased_hole_take_full,
#       ac3615_3_soft_zero_extra_path, ac3615_4_no_new_metrics_or_query_key,
#       ac3615_5_linter_self_test).
# AC5: build.py wires this linter + scripts/coverage/root_check_allowlist.txt.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SVC = "src/compiler/service.ixx"
HEALTH = "tests/compiler/test_dep_graph_hybrid_cascade.cpp"
BUILD = "build.py"
QUERY = "src/compiler/evaluator_primitives_obs_eval.cpp"
OBS = "src/compiler/observability_metrics.h"
ALLOW = "scripts/coverage/root_check_allowlist.txt"

SITE = "Issue #3615"
CONE = "fail_closed_soft_dual_graph_parity_before_partial_cone_"
OLD_FRONT = "auto eit = ir_cache_v2_.find(dirty_names.front());"
PROD = "production_defaults_active()"
FULL = "AuditStrategy::Full"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(svc: str, health: str, build: str, query: str, obs: str, allow: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    cone_pos = svc.find(CONE)
    if cone_pos == -1:
        fails.append("AC1: cone helper missing")
        win = ""
    else:
        # Window starts 1500 chars before cone_pos to capture the function
        # header comment (which contains "Issue #3615" cite) plus the
        # function body. The comment block is ~15 lines × ~70 chars.
        start = cone_pos - 1500 if cone_pos > 1500 else 0
        win = svc[start : cone_pos + 6500]
    must(SITE, "AC1 cone helper cite", win)
    must("graphs_consistent", "AC1 cone helper parity check", win)
    must("node_dep_graph_.dependents", "AC1 Soft-erased hole check", win)
    must("encode_fn_node", "AC1 NodeId encode", win)
    must("rebuild_node_dep_graph_from_string", "AC1 rebuild", win)
    must("dual_dep_graph_parity_fail_total", "AC1 fail counter", win)
    must("partial_forced_full_by_impact_total", "AC1 forced-full distinguisher", win)
    must("OrderedSharedLock", "AC1 shared lock on consistent path", win)
    must("OrderedUniqueLock", "AC1 exclusive lock on rebuild", win)
    must(PROD, "AC1 production gate", win)
    must(FULL, "AC1 Full gate", win)

    rel_pos = svc.find("std::size_t relower_dirty_defines_from_workspace()")
    if rel_pos == -1:
        fails.append("AC1: relower function missing")
        rel_win = ""
    else:
        rel_win = svc[rel_pos : rel_pos + 12000]
        must(CONE, "AC1 relower uses cone helper", rel_win)
        must(SITE, "AC1 relower cite", rel_win)
        must_not(OLD_FRONT, "AC1 old front-only block removed", rel_win)

    must(PROD, "AC2 production gate in helper", win)

    must_not("3615", "AC3 no new metrics field", obs)
    must_not("schema-3615", "AC3 no new query key", query)
    if (ROOT / "tests" / "compiler" / "test_issue_3615.cpp").is_file():
        fails.append("AC3: forbidden tests/compiler/test_issue_3615.cpp (#81934)")
    if (ROOT / "tests" / "issues" / "test_issue_3615.cpp").is_file():
        fails.append("AC3: forbidden tests/issues/test_issue_3615.cpp (#81934)")
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir():
        for p in design_dir.glob("*"):
            if "3615" in p.name:
                fails.append(f"AC3: forbidden docs/design/{p.name} (#1655)")
                break

    must("ac3615_1_cone_wide_non_front_fork", "AC4 cone AC", health)
    must("ac3615_2_soft_erased_hole_take_full", "AC4 Soft-erased AC", health)
    must("ac3615_3_soft_zero_extra_path", "AC4 Soft AC", health)
    must("ac3615_4_no_new_metrics_or_query_key", "AC4 no-invent AC", health)
    must("ac3615_5_linter_self_test", "AC4 linter AC", health)
    must(SITE, "AC4 test cite", health)

    must("check_dual_graph_parity_cone_3615", "AC5 build.py wiring", build)
    must("check_dual_graph_parity_cone_3615.py", "AC5 allowlist entry", allow)
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        sample_svc = (
            SITE + "\n" + CONE + "(cone_names, want)\n"
            "graphs_consistent\n"
            "node_dep_graph_.dependents\n"
            "encode_fn_node\n"
            "rebuild_node_dep_graph_from_string\n"
            "dual_dep_graph_parity_fail_total\n"
            "partial_forced_full_by_impact_total\n"
            "OrderedSharedLock\n"
            "OrderedUniqueLock\n" + PROD + "\n" + FULL + "\n"
            "std::size_t relower_dirty_defines_from_workspace()\n" + CONE + "\n" + SITE + "\n"
        )
        sample_health = (
            "ac3615_1_cone_wide_non_front_fork\n"
            "ac3615_2_soft_erased_hole_take_full\n"
            "ac3615_3_soft_zero_extra_path\n"
            "ac3615_4_no_new_metrics_or_query_key\n"
            "ac3615_5_linter_self_test\n" + SITE
        )
        ok_fails = _rows(
            sample_svc,
            sample_health,
            "check_dual_graph_parity_cone_3615",
            "clean",
            "clean",
            "check_dual_graph_parity_cone_3615.py",
        )
        if ok_fails:
            for f in ok_fails:
                print(f"self-test: unexpected failure on positive sample: {f}")
            return 1
        neg_fails = _rows("", "", "", "", "", "")
        if len(neg_fails) < 10:
            print(f"self-test: negative sample only fired {len(neg_fails)} rows")
            return 1
        print("self-test: ok")
        return 0

    fails = _rows(_read(SVC), _read(HEALTH), _read(BUILD), _read(QUERY), _read(OBS), _read(ALLOW))
    if fails:
        for f in fails:
            print(f"FAIL {f}")
        if args.strict:
            return 1
        print("(non-strict: reporting only)")
        return 0
    print("dual graph parity cone (#3615) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
