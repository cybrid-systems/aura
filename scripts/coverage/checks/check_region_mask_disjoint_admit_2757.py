#!/usr/bin/env python3
"""Issue #2757: region concurrent mask-AND disjointness (refine #2724 residual).

Extends #2754 (equal-key cone path) so proven ImpactScope/dirty masks with
empty intersection concurrent-admit even when region_keys collide or are
zero. Quiet path (no masks) remains identical to #2724 equality only.

Contract (one row per AC):
  AC1 Production + mask-AND empty intersection → concurrent admit even
     when keys collide or are zero (regions_mask_disjoint + counter).
  AC2 Production + overlapping masks → region-overlap reject; densify
     isolation preserved.
  AC3 Soft / sandbox=off → metric-only.
  AC4 Quiet path (either mask==0) → #2724 equality only (zero extra work).
  AC5 Additive mutation-region-mask-disjoint-admit-total + schema-2757;
     all #2724/#2754 keys preserved; no docs/design/*.
  AC6 Extend test_mailbox_hold_starvation_hard.cpp; linter wired.

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
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    mhb = _read("src/compiler/mutation_hold_budget.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    # AC1 — zero-key + mask-AND.
    must("Issue #2757", "AC1", mhb)
    must("regions_mask_disjoint", "AC1", mhb)
    must("g_mutation_region_mask_disjoint_admit_total", "AC1", mhb)
    must("kMutationRegionMaskDisjointIssue = 2757", "AC1", mhb)
    must("region_or_mask", "AC1", emb)
    must("regions_mask_disjoint", "AC1", emb)
    must("g_mutation_region_mask_disjoint_admit_total.fetch_add(1,", "AC1", emb)
    must("return a != 0 && b != 0 && a != b;", "AC1 key fast path", mhb)

    # AC2 — overlap + densify.
    must("AdmissionRejected: region-overlap", "AC2", emb)
    must("atomic_batch_active", "AC2", emb)
    must("region_shard_", "AC2", emb)

    # AC3/AC4 — soft + quiet.
    must("metric-only", "AC3", emb)
    must("if (mask_a == 0 || mask_b == 0)", "AC4 quiet", mhb)
    must("region_or_mask", "AC4", emb)

    # AC5 — query keys + prior surfaces.
    must("Issue #2757", "AC5", q)
    must_key("mutation-region-mask-disjoint-admit-total", "AC5", q)
    must_key("mutation-region-mask-disjoint-wired", "AC5", q)
    must('"schema-2757"', "AC5", q)
    must('"issue-2757"', "AC5", q)
    must_key("mutation-region-concurrent-admit-total", "AC5 #2724", q)
    must_key("mutation-region-concurrent-cone-admit-total", "AC5 #2754", q)
    must('"schema-2724"', "AC5", q)
    must('"schema-2754"', "AC5", q)

    # AC6 — tests + linter + no docs/design.
    must("ac2757_1_zero_key_mask_disjoint_admit", "AC6", t)
    must("ac2757_2_overlap_and_densify", "AC6", t)
    must("ac2757_3_soft_and_quiet_path", "AC6", t)
    must("ac2757_5_additive_observability", "AC6", t)
    must("ac2757_6_source_and_linter", "AC6", t)
    must("ac2754_1_cone_disjoint_concurrent_admit", "AC6 #2754 preserved", t)
    must("check_region_mask_disjoint_admit_2757", "AC6", build)
    if (ROOT / "tests" / "serve" / "test_issue_2757.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_2757.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2757-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2757 region mask-disjoint concurrent admit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
