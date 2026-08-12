#!/usr/bin/env python3
"""Issue #2847: bind concurrent region admit to type/occurrence commit gate.

Residual of #2724/#2760/#2761: region concurrent admit isolates AST topology
but shared TypeChecker CS / occurrence_goals_ can still cross-talk. Production
rejects when touched OccurrenceGoal predicate bits fall outside admitted mask.

Contract (one row per AC):
  AC1 region_type_commit_ok pure helper; node_id_to_region_mask_bit; mask==0 ok
  AC2 Soft observe + production reject counters; force_reason code 13
  AC3 Soft never hard-rejects solely on this face (observe-only note)
  AC4 admitted_cone_mask_ != 0 gate (quiet/global zero-cost class)
  AC5 additive query schema-2847; #2724/#2761 surfaces preserved
  AC6 ac2847_* tests + this linter; no docs/design/*; no invent test

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

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

    ta = _read("src/compiler/typed_mutation_audit.h")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    eix = _read("src/compiler/evaluator.ixx")
    q = read_query_prims()
    t = _read("tests/serve/test_mailbox_hold_starvation_hard.cpp")
    build = _read("build.py")

    # AC1 — pure helper
    must("region_type_commit_ok", "AC1", ta)
    must("node_id_to_region_mask_bit", "AC1", ta)
    must("kRegionTypeCrossTalkIssue = 2847", "AC1", ta)
    must("if (admitted_mask == 0)", "AC1 quiet", ta)
    must("if (touched_type_mask == 0)", "AC1 no-type-work", ta)
    must("(touched_type_mask & ~admitted_mask) == 0", "AC1 in-cone", ta)

    # AC2 — Soft/production faces + force_reason
    must("g_region_type_cross_talk_observe_total", "AC2", ta)
    must("g_region_type_cross_talk_reject_total", "AC2", ta)
    must("note_region_type_cross_talk", "AC2", ta)
    must("region_type_cross_talk", "AC2 reason", ta)
    must("return 13", "AC2 code 13", ta)
    must("note_region_type_cross_talk", "AC2", emb)
    must("#2847", "AC2", emb)

    # AC3 — Soft observe only when !production_hard (hard=false path)
    must("note_region_type_cross_talk(hard)", "AC3 emb note", emb)
    must("Soft: observe only", "AC3 Soft comment", emb)
    # Soft note path: observe counter only when !hard
    must("g_region_type_cross_talk_observe_total.fetch_add", "AC3 Soft observe", ta)

    # AC4 — quiet path gate
    must("admitted_cone_mask_ != 0", "AC4", emb)
    must("admitted_cone_mask_", "AC4", eix)
    must("admitted_region_key_", "AC4", eix)

    # AC5 — query + prior surfaces
    must_key("region-type-cross-talk-observe-total", "AC5", q)
    must_key("region-type-cross-talk-reject-total", "AC5", q)
    must_key("schema-2847", "AC5", q)
    must_key("mutation-region-concurrent-admit-total", "AC5 #2724", q)
    must_key("mutation-region-mask-overlap-reject-total", "AC5 #2761", q)
    must("g_mutation_region_concurrent_admit_total", "AC5 emb #2724", emb)

    # AC6 — tests + linter
    must("ac2847_1_pure_commit_ok", "AC6", t)
    must("ac2847_2_soft_observe_production_reject", "AC6", t)
    must("ac2847_4_zero_cost_quiet", "AC6", t)
    must("ac2847_5_additive_and_preserved", "AC6", t)
    must("ac2847_6_source_and_linter", "AC6", t)
    must("check_region_type_commit_gate_2847", "AC6", build)
    must("ac2761_1_unequal_key_mask_overlap_reject", "AC6 #2761 preserved", t)
    must("ac2724_1_disjoint_concurrent_admit", "AC6 #2724 preserved", t)
    if (ROOT / "tests" / "serve" / "test_issue_2847.cpp").is_file():
        fails.append("AC6: test_issue_2847.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2847*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2847 region type/occurrence commit gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
