#!/usr/bin/env python3
# scripts/coverage/checks/check_cascade_rearm_new_edge_only_3168.py -- Issue #3168 source-cite gate.
#
# Verifies the residual close-out for multi-round AI self-modify — concurrent
# cascade re-arm under production multi-fiber must prefer new-edge-only mark
# over mark_all_blocks_dirty. Wire-up spans four layers:
#
#  1. Counter field (src/compiler/observability_metrics.h, struct end — AC6
#     layout-stable per #2906):
#     - cascade_rearm_new_edge_only_total: bumped when attribution succeeds
#
#  2. Critical-section extension (src/compiler/service.ixx, in
#    relower_dirty_defines_from_workspace, after the cascade_decision_mtx_
#    acquisition per #3135):
#     - initial_deferred_edges_size snapshotted under shared dep_graph_mtx_
#     - In the rearm_observed_mid_loop branch (when want_partial && rearmed):
#       - Walk [initial_deferred_edges_size, current) under shared
#         dep_graph_mtx_ and snapshot the new-edge range
#       - For edges touching THIS define (caller or callee == name), mark
#         only those blocks via mark_block_dirty (precise, idempotent)
#       - Bump cascade_rearm_new_edge_only_total + keep want_partial true
#         (partial peel preserved)
#     - Defensive fallback (new-edge set empty / cannot be attributed):
#       existing mark_all_blocks_dirty + partial_forced_full_by_impact_total
#       bump preserved per #3097
#
#  3. Test extension (tests/compiler/test_cascade_decision_residual_atomic.cpp):
#     - AC1 production concurrent re-arm → new-edge-only attribution
#     - AC2 Soft/Off + single-fiber + clean (armed==0) → zero extra
#     - AC3 Attribution success → partial peel preserved
#     - AC4 #3067 (drain at entry) + #3097 (impact_ub consult + counter)
#       + #3135 (cascade_decision_mtx_ + defer_lock) preserved
#     - AC5 Quiet happy path (no concurrent reject) → no extra work
#     - AC8 source-cite + coverage linter + build.py wiring
#
#  4. Forbidden artifacts (per #1655 + #81967):
#     - No docs/design/3168-* plan doc
#     - No tests/issues/test_issue_3168.cpp
#     - No tests/compiler/test_issue_3168.cpp
#     - No tests/serve/test_issue_3168.cpp

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/observability_metrics.h",
    "src/compiler/service.ixx",
    "tests/compiler/test_cascade_decision_residual_atomic.cpp",
)

# (path, regex, label) -- each tuple is a regex pattern that must appear.
INFRA_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # Counter field at struct end (AC6 layout-stable per #2906).
    (
        "src/compiler/observability_metrics.h",
        r"std::atomic<std::uint64_t>\s+cascade_rearm_new_edge_only_total\{0\}",
        "obs: cascade_rearm_new_edge_only_total counter",
    ),
    (
        "src/compiler/observability_metrics.h",
        r"//\s*Issue #3168:.*concurrent cascade re-arm under production multi-fiber",
        "obs: counter declaration cites #3168",
    ),
    # Snapshot block — must sit AFTER the cascade_decision_mtx_ lock
    # acquisition so Soft/Off + clean (need_lock==false) skips it (AC2).
    (
        "src/compiler/service.ixx",
        r"Issue #3168: snapshot deferred_hybrid_edges_\.size\(\)",
        "service: snapshot block cites #3168",
    ),
    (
        "src/compiler/service.ixx",
        r"initial_deferred_edges_size\s*=\s*deferred_hybrid_edges_\.size\(\)",
        "service: initial_deferred_edges_size snapshotted",
    ),
    (
        "src/compiler/service.ixx",
        r"OrderedSharedLock<std::shared_mutex>\s+read\(\s*dep_graph_mtx_",
        "service: snapshot uses dep_graph_mtx_ shared lock",
    ),
    # Attribution block.
    (
        "src/compiler/service.ixx",
        r"Issue #3168: prefer new-edge-only mark over full fallback",
        "service: attribution block cites #3168",
    ),
    (
        "src/compiler/service.ixx",
        r"new_edges_snapshot\.push_back\(deferred_hybrid_edges_\[idx\]\)",
        "service: new-edge range walked under shared dep_graph_mtx_",
    ),
    (
        "src/compiler/service.ixx",
        r"it->second\.mark_block_dirty\(fi,\ bi\)",
        "service: precise mark_block_dirty used in attribution",
    ),
    (
        "src/compiler/service.ixx",
        r"metrics_\.cascade_rearm_new_edge_only_total\.fetch_add\(\s*1",
        "service: counter bumped in attribution block",
    ),
    # Defensive fallback preserves #3097 semantics.
    (
        "src/compiler/service.ixx",
        r"//\s*Defensive last-resort: new-edge set empty or",
        "service: defensive fallback comment",
    ),
    (
        "src/compiler/service.ixx",
        r"metrics_\.partial_forced_full_by_impact_total\.fetch_add\(\s*1",
        "service: defensive fallback bumps #3097 counter",
    ),
    # Existing cascade / dep-graph / mutate contracts preserved.
    (
        "src/compiler/service.ixx",
        r"drain_deferred_hybrid_cascade_\(\)",
        "service: #3067 drain at entry preserved",
    ),
    (
        "src/compiler/service.ixx",
        r"impact_upper_bound_for_entry_\(",
        "service: #3097 impact_ub consult preserved",
    ),
    (
        "src/compiler/service.ixx",
        r"cascade_decision_mtx_",
        "service: #3135 cascade_decision_mtx_ preserved",
    ),
    (
        "src/compiler/service.ixx",
        r"graph_grew_mid_loop",
        "service: #3161 graph_grew_mid_loop observation preserved",
    ),
    # Test extension: AC1..AC8 source-cite.
    (
        "tests/compiler/test_cascade_decision_residual_atomic.cpp",
        r"ac3168_1_production_rearm_new_edge_only",
        "test: AC1 production rearm new-edge-only",
    ),
    (
        "tests/compiler/test_cascade_decision_residual_atomic.cpp",
        r"ac3168_2_soft_zero_extra",
        "test: AC2 soft zero extra",
    ),
    (
        "tests/compiler/test_cascade_decision_residual_atomic.cpp",
        r"ac3168_3_partial_peel_preserved",
        "test: AC3 partial peel preserved",
    ),
    (
        "tests/compiler/test_cascade_decision_residual_atomic.cpp",
        r"ac3168_4_existing_3067_3097_3135_preserved",
        "test: AC4 existing 3067/3097/3135 preserved",
    ),
    (
        "tests/compiler/test_cascade_decision_residual_atomic.cpp",
        r"ac3168_5_source_and_linter",
        "test: AC8 source/linter suite",
    ),
    # Build.py wiring.
    (
        "build.py",
        r"cmd_cascade_rearm_new_edge_only_3168",
        "build: cmd_cascade_rearm_new_edge_only_3168 dispatcher",
    ),
    (
        "build.py",
        r"check_cascade_rearm_new_edge_only_3168",
        "build: linter path wired",
    ),
)

FORBIDDEN_DOCS: tuple[str, ...] = (
    "docs/design/3168-cascade-rearm-new-edge.md",
    "docs/design/3168-cascade-rearm-new-edge-only.md",
    "docs/design/3168-concurrent-cascade-rearm.md",
)

FORBIDDEN_TESTS: tuple[str, ...] = (
    "tests/issues/test_issue_3168.cpp",
    "tests/compiler/test_issue_3168.cpp",
    "tests/serve/test_issue_3168.cpp",
    "tests/core/test_issue_3168.cpp",
)


def check_file(rel: str, rx: str, strict: bool = True) -> list[str]:
    p = REPO_ROOT / rel
    if not p.exists():
        return [f"MISSING: {rel}"]
    text = p.read_text(encoding="utf-8", errors="replace")
    if re.search(rx, text, re.MULTILINE | re.DOTALL):
        return []
    if strict:
        return [f"MISSING PATTERN: {rel} :: {rx}"]
    return []


def check_no_forbidden_artifacts() -> list[str]:
    failures: list[str] = []
    for rel in FORBIDDEN_DOCS + FORBIDDEN_TESTS:
        p = REPO_ROOT / rel
        if p.exists():
            failures.append(f"FORBIDDEN ARTIFACT: {rel}")
    return failures


def _self_test() -> int:
    fixture = """
    // src/compiler/observability_metrics.h
    struct CompilerMetrics {
        // ... existing counters ...
        // Issue #3168: concurrent cascade re-arm under production multi-fiber
        std::atomic<std::uint64_t> cascade_rearm_new_edge_only_total{0};
    };
    // src/compiler/service.ixx
    if (need_lock)
        cascade_guard.lock();
    // Issue #3168: snapshot deferred_hybrid_edges_.size()
    std::size_t initial_deferred_edges_size = 0;
    {
        lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_, lock_order::Level::DepGraph);
        initial_deferred_edges_size = deferred_hybrid_edges_.size();
    }
    if (rearm_observed_mid_loop && want_partial) {
        // Issue #3168: prefer new-edge-only mark over full fallback
        std::vector<std::pair<std::string, std::string>> new_edges_snapshot;
        {
            OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_, ...);
            if (deferred_hybrid_edges_.size() > initial_deferred_edges_size) {
                new_edges_snapshot.reserve(...);
                for (std::size_t idx = initial_deferred_edges_size;
                     idx < deferred_hybrid_edges_.size(); ++idx) {
                    new_edges_snapshot.push_back(deferred_hybrid_edges_[idx]);
                }
            }
        }
        for (fi, bi) it->second.mark_block_dirty(fi, bi);
        metrics_.cascade_rearm_new_edge_only_total.fetch_add(1, std::memory_order_relaxed);
        // Defensive last-resort: new-edge set empty or
        // cannot be attributed.
        metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
        drain_deferred_hybrid_cascade_();
        impact_upper_bound_for_entry_(name, it->second);
        cascade_decision_mtx_;
        graph_grew_mid_loop;
    }
    // tests/compiler/test_cascade_decision_residual_atomic.cpp
    ac3168_1_production_rearm_new_edge_only();
    ac3168_2_soft_zero_extra();
    ac3168_3_partial_peel_preserved();
    ac3168_4_existing_3067_3097_3135_preserved();
    ac3168_5_source_and_linter();
    // build.py
    cmd_cascade_rearm_new_edge_only_3168
    check_cascade_rearm_new_edge_only_3168
    """
    failures: list[str] = []
    for rel, rx, _label in INFRA_REQUIRED:
        if not re.search(rx, fixture, re.MULTILINE | re.DOTALL):
            failures.append(f"SELF-TEST: pattern missing for {rel} :: {rx}")
    if failures:
        print("SELF-TEST FAIL:")
        for line in failures:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns found in fixture")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3168 concurrent cascade re-arm new-edge-only source-cite gate",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=True,
        help="Fail on missing patterns (default: strict)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Validate linter regex / structure against fixture text",
    )
    parser.add_argument(
        "--forbidden-only",
        action="store_true",
        help="Only check forbidden artifacts (docs/design/3168-*, test_issue_3168.cpp)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures: list[str] = []

    if not args.forbidden_only:
        for rel, rx, _label in INFRA_REQUIRED:
            failures.extend(check_file(rel, rx, strict=args.strict))

    failures.extend(check_no_forbidden_artifacts())

    if failures:
        print(f"FAIL: {len(failures)} issue(s):")
        for line in failures:
            print(f"  {line}")
        return 1

    print("OK: Issue #3168 source-cite + coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
