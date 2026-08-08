#!/usr/bin/env python3
"""Issue #2761: mask-AND sole authority for concurrent region admit.

Closes #2724 residual: unequal region_keys that still overlap in AST cone /
ImpactScope / dirty impact must NOT concurrent-admit. When both cone masks
are proven, mask-AND is sole authority; missing mask falls back to #2724
key-equality (no tree walk).

Contract (one row per AC):
  AC1 Production + unequal keys + overlapping proven masks → reject
     region-overlap; mask-overlap-reject counter bumps.
  AC2 Production + unequal keys + proven-disjoint masks → concurrent admit
     (preserve #2724 throughput when cones truly disjoint).
  AC3 Soft / sandbox=off → metric-only.
  AC4 Hot path: mask AND only; missing mask → key-equality fallback.
  AC5 Additive mutation-region-mask-overlap-reject-total + schema-2761;
     all #2724/#2754/#2757/#2760/#2701 surfaces preserved; no docs/design/*.
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
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    # AC1 — mask-first + unequal-key overlap reject.
    must("Issue #2761", "AC1", mhb)
    must("kMutationRegionMaskOverlapIssue = 2761", "AC1", mhb)
    must("regions_mask_overlap", "AC1", mhb)
    must("g_mutation_region_mask_overlap_reject_total", "AC1", mhb)
    must("if (mask_a != 0 && mask_b != 0)", "AC1 mask-first", mhb)
    must("(mask_a & mask_b) == 0", "AC1 mask-AND", mhb)
    must("regions_mask_overlap", "AC1", emb)
    must("g_mutation_region_mask_overlap_reject_total.fetch_add(1,", "AC1", emb)
    must("AdmissionRejected: region-overlap", "AC1", emb)
    # Must NOT prefer key-disjoint before mask when masks proven
    # (the old residual: if (a != 0 && b != 0 && a != b) return true first).
    # Extract 4-arg regions_disjoint body roughly.
    sig = "bool regions_disjoint(std::uint64_t a, std::uint64_t b, std::uint64_t mask_a"
    i = mhb.find(sig)
    if i < 0:
        fails.append("AC1: 4-arg regions_disjoint not found")
    else:
        brace = mhb.find("{", i)
        depth = 0
        body = ""
        for j in range(brace, len(mhb)):
            if mhb[j] == "{":
                depth += 1
            elif mhb[j] == "}":
                depth -= 1
                if depth == 0:
                    body = mhb[brace + 1 : j]
                    break
        mask_idx = body.find("mask_a != 0 && mask_b != 0")
        key_idx = body.find("a != 0 && b != 0 && a != b")
        if mask_idx < 0:
            fails.append("AC1: 4-arg body missing proven-masks gate")
        elif key_idx >= 0 and key_idx < mask_idx:
            fails.append("AC1 residual: key-disjoint must NOT precede mask-AND when masks proven")

    # AC2 — disjoint still admits (helpers present; live unit in tests).
    must("regions_mask_disjoint", "AC2", mhb)
    must("regions_disjoint", "AC2", emb)

    # AC3 — soft.
    must("metric-only", "AC3", emb)
    must("g_last_admitted_cone_mask_soft", "AC3", emb)

    # AC4 — hot path + fallback.
    must("(mask_a & mask_b) == 0", "AC4", mhb)
    must("return a != 0 && b != 0 && a != b;", "AC4 key fallback", mhb)
    if "tree walk" not in mhb and "bit AND" not in mhb and "Hot path is a bit AND" not in mhb:
        fails.append("AC4: document no tree walk / bit AND hot path")

    # AC5 — query + prior.
    must("Issue #2761", "AC5", q)
    must_key("mutation-region-mask-overlap-reject-total", "AC5", q)
    must_key("mutation-region-mask-overlap-wired", "AC5", q)
    must('"schema-2761"', "AC5", q)
    must('"issue-2761"', "AC5", q)
    must_key("mutation-region-concurrent-admit-total", "AC5 #2724", q)
    must_key("mutation-region-overlap-reject-total", "AC5 #2724", q)
    must_key("mutation-region-concurrent-cone-admit-total", "AC5 #2754", q)
    must_key("mutation-region-mask-disjoint-admit-total", "AC5 #2757", q)
    must_key("mutation-region-impact-mask-admit-total", "AC5 #2760", q)
    must('"schema-2724"', "AC5", q)
    must('"schema-2754"', "AC5", q)
    must('"schema-2757"', "AC5", q)
    must('"schema-2760"', "AC5", q)

    # AC6 — tests + linter.
    must("ac2761_1_unequal_key_mask_overlap_reject", "AC6", t)
    must("ac2761_2_unequal_key_mask_disjoint_admit", "AC6", t)
    must("ac2761_3_soft_and_hot_path", "AC6", t)
    must("ac2761_5_additive_observability", "AC6", t)
    must("ac2761_6_source_and_linter", "AC6", t)
    must("ac2760_1_impact_mask_concurrent_admit", "AC6 #2760 preserved", t)
    must("check_region_mask_overlap_admit_2761", "AC6", build)
    if (ROOT / "tests" / "serve" / "test_issue_2761.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_2761.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2761-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2761 mask-AND sole authority (unequal-key cone overlap reject) — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
