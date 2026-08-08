#!/usr/bin/env python3
"""Issue #2754: region concurrent cone / ImpactScope mask-AND disjointness
(#2724 residual).

Contract (one row per AC):
  AC1 Production + equal region keys but cone-/mask-disjoint ImpactScope
     → concurrent admit (bump mutation-region-concurrent-cone-admit-total).
     Key-equality remains the fast path (2-arg regions_disjoint);
     4-arg regions_disjoint + regions_cone_disjoint use mask AND only
     (no tree walk). TLS cone mask via note_parallel_task_cone_mask /
     parallel_task_cone_mask (same pattern as #2746 region_key TLS).
  AC2 Production + true overlapping cones (or mask==0 unknown) → still
     reject AdmissionRejected: region-overlap; overlap counter bumps.
  AC3 Soft / sandbox=off → metric-only observation (no lock regression);
     soft path tracks last cone mask for observability.
  AC4 densify / ownership_rebind / restamp remain correct under concurrent
     region holds (per-region shard + atomic_batch GlobalExclusive
     fallback preserved — #2121 contract).
  AC5 Additive observability — mutation-region-concurrent-cone-admit-total
     + mutation-region-cone-disjoint-wired + schema-2754 / issue-2754.
     All #2701 / #2720 / #2724 / #2726 / #2551 / #2587 surfaces preserved.
  AC6 Source-cite + extend tests/serve/test_mailbox_hold_starvation_hard.cpp
     per #81967 (no new test file). This linter wired in build.py.
     No docs/design/2754-* per #1655.

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

    def must_key(n: str, label: str, hay: str) -> None:
        # clang-format may split string literals; strip quotes + whitespace.
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    mhb = _read("src/compiler/mutation_hold_budget.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    # AC1 — cone-disjoint concurrent admit.
    must("Issue #2754", "AC1", mhb)
    must("g_mutation_region_concurrent_cone_admit_total", "AC1", mhb)
    must("kMutationRegionConeDisjointIssue = 2754", "AC1", mhb)
    must("regions_cone_disjoint", "AC1", mhb)
    must("(mask_a & mask_b) == 0", "AC1 mask-AND", mhb)
    must("return a != 0 && b != 0 && a != b;", "AC1 key fast path", mhb)
    must("regions_disjoint(region_key, g_last_admitted_region_key, cone_mask", "AC1", emb)
    must("regions_cone_disjoint", "AC1", emb)
    must("g_mutation_region_concurrent_cone_admit_total.fetch_add(1,", "AC1", emb)
    must("parallel_task_cone_mask()", "AC1", emb)
    must("note_parallel_task_cone_mask", "AC1", efm)
    must("g_parallel_task_cone_mask", "AC1", efm)
    must("note_parallel_task_cone_mask", "AC1", ixx)
    must("parallel_task_cone_mask()", "AC1", ixx)

    # AC2 — true overlap still rejects.
    must("AdmissionRejected: region-overlap", "AC2", emb)
    must("g_mutation_region_overlap_reject_total.fetch_add(1,", "AC2", emb)
    must("mask_a != 0 && mask_b != 0", "AC2 unknown-mask conservative", mhb)

    # AC3 — soft metric-only.
    must("g_last_admitted_cone_mask_soft", "AC3", emb)
    must("metric-only", "AC3", emb)

    # AC4 — densify / shard isolation preserved.
    must("region_shard_", "AC4", emb)
    must("workspace_region_shard", "AC4", emb)
    must("atomic_batch_active", "AC4", emb)
    must("workspace_region_fallback_global_total", "AC4", emb)

    # AC5 — additive query keys + prior surfaces.
    must("Issue #2754", "AC5", q)
    must_key("mutation-region-concurrent-cone-admit-total", "AC5", q)
    must_key("mutation-region-cone-disjoint-wired", "AC5", q)
    must('"schema-2754"', "AC5", q)
    must('"issue-2754"', "AC5", q)
    must_key("mutation-region-concurrent-admit-total", "AC5 #2724", q)
    must_key("mutation-region-overlap-reject-total", "AC5 #2724", q)
    must('"schema-2724"', "AC5", q)
    must('"schema-2701"', "AC5", q)
    must('"schema-2720"', "AC5", q)
    must('"schema-2726"', "AC5", q)

    # AC6 — source-cite + test extension + linter + no docs/design.
    must("ac2754_1_cone_disjoint_concurrent_admit", "AC6", t)
    must("ac2754_2_true_overlap_still_rejects", "AC6", t)
    must("ac2754_3_soft_path_metric_only", "AC6", t)
    must("ac2754_4_densify_under_concurrent_holds", "AC6", t)
    must("ac2754_5_additive_observability", "AC6", t)
    must("ac2754_6_source_and_linter", "AC6", t)
    must("ac2724_1_disjoint_concurrent_admit", "AC6 #2724 preserved", t)
    if (ROOT / "tests" / "serve" / "test_issue_2754.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_2754.cpp present (forbidden per #81967)")
    must("check_region_cone_disjoint_admit_2754", "AC6", build)
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2754-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2754 region cone-disjoint concurrent admit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
