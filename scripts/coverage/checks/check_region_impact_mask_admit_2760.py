#!/usr/bin/env python3
"""Issue #2760: ImpactScope / dirty-bit mask-AND production enablement.

Refines #2724 residual after #2754/#2757: regions_disjoint already does
mask-AND; #2760 wires proven ImpactScope/dirty-bit masks into commercial
multi-agent paths (parallel-intend :cone-masks, effective_region_cone_mask,
impact_block_to_region_mask_bit) so concurrent admit is real under
multi-hypothesis load — not only key-equality.

Contract (one row per AC):
  AC1 Production + proven-disjoint ImpactScope/dirty masks → concurrent
     admit (impact-mask-admit + mask-AND); overlapping → region-overlap.
  AC2 Soft / sandbox=off → metric-only (no lock regression).
  AC3 Happy path (no key, no mask) → zero extra cost (region_or_mask gate).
  AC4 densify / region_shard / atomic_batch GlobalExclusive preserved.
  AC5 Additive mutation-region-impact-mask-admit-total + schema-2760;
     all #2724/#2754/#2757/#2701 surfaces preserved; no docs/design/*.
  AC6 Extend test_mailbox_hold_starvation_hard.cpp; this linter wired.

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
    agent = _read("src/compiler/evaluator_primitives_agent.cpp")
    orch = _read("src/serve/parallel_orch.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    # AC1 — ImpactScope mask helpers + production wire + mask-AND.
    must("Issue #2760", "AC1", mhb)
    must("effective_region_cone_mask", "AC1", mhb)
    must("impact_block_to_region_mask_bit", "AC1", mhb)
    must("region_key_as_impact_mask", "AC1", mhb)
    must("g_mutation_region_impact_mask_admit_total", "AC1", mhb)
    must("kMutationRegionImpactMaskIssue = 2760", "AC1", mhb)
    must("(mask_a & mask_b) == 0", "AC1 mask-AND", mhb)
    must("return a != 0 && b != 0 && a != b;", "AC1 key fast path", mhb)
    must("effective_region_cone_mask", "AC1", emb)
    must("g_mutation_region_impact_mask_admit_total.fetch_add(1,", "AC1", emb)
    must("AdmissionRejected: region-overlap", "AC1", emb)
    must("note_parallel_task_cone_mask", "AC1", agent)
    must("cone_mask", "AC1", orch)
    if "cone-masks" not in agent and "cone_masks" not in agent:
        fails.append("AC1: parallel-intend must accept cone-masks")

    # AC2 — soft metric-only.
    must("metric-only", "AC2", emb)
    must("g_last_admitted_cone_mask_soft", "AC2", emb)

    # AC3 — quiet path.
    must("region_or_mask", "AC3", emb)
    must("if (tls_cone_mask != 0)", "AC3", mhb)

    # AC4 — densify isolation.
    must("region_shard_", "AC4", emb)
    must("atomic_batch_active", "AC4", emb)
    must("workspace_region_fallback_global_total", "AC4", emb)

    # AC5 — query + prior surfaces.
    must("Issue #2760", "AC5", q)
    must_key("mutation-region-impact-mask-admit-total", "AC5", q)
    must_key("mutation-region-impact-mask-wired", "AC5", q)
    must('"schema-2760"', "AC5", q)
    must('"issue-2760"', "AC5", q)
    must_key("mutation-region-concurrent-admit-total", "AC5 #2724", q)
    must_key("mutation-region-concurrent-cone-admit-total", "AC5 #2754", q)
    must_key("mutation-region-mask-disjoint-admit-total", "AC5 #2757", q)
    must('"schema-2724"', "AC5", q)
    must('"schema-2754"', "AC5", q)
    must('"schema-2757"', "AC5", q)

    # AC6 — tests + linter + no docs/design.
    must("ac2760_1_impact_mask_concurrent_admit", "AC6", t)
    must("ac2760_2_soft_and_quiet", "AC6", t)
    must("ac2760_4_densify_isolation", "AC6", t)
    must("ac2760_5_additive_observability", "AC6", t)
    must("ac2760_6_source_and_linter", "AC6", t)
    must("ac2757_1_zero_key_mask_disjoint_admit", "AC6 #2757 preserved", t)
    must("ac2754_1_cone_disjoint_concurrent_admit", "AC6 #2754 preserved", t)
    must("check_region_impact_mask_admit_2760", "AC6", build)
    if (ROOT / "tests" / "serve" / "test_issue_2760.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_2760.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2760-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2760 region ImpactScope/dirty-bit mask-AND admit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
