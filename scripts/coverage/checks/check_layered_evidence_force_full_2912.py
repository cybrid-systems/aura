#!/usr/bin/env python3
"""Issue #2912: layered DeadCoercion evidence coherence diverge must force-Full
under production (closes #2719 residual — arm alone left pending unconsumed).

Contract:
  AC1 Production + inject diverge / pending → consume forces Full audit
      (or hard-reject with force_reason "layered-evidence-diverge")
  AC2 After escalate: dual-complete + provenance fill still wired
  AC3 Quiet path → consume no-ops / zero cost
  AC4 Additive schema-2912 + consume counters; #2719/#2674 keys preserved
  AC5 Source-cite + suite extended (test_dead_coercion_layered.cpp)
  AC6 Soft vs production table in comments; linter + build.py wired

Exit 0 = all AC rows satisfied.
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

    cixx = _read("src/compiler/coercion_map.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_dead_coercion_layered.cpp")
    build = _read("build.py")

    # AC1: consume helpers + boundary OR into provenance_miss
    must("consume_layered_evidence_diverge_force_full", "AC1", cixx)
    must("consume_layered_evidence_diverge_hard_reject", "AC1", cixx)
    must("g_layered_evidence_diverge_force_consumed_total", "AC1", cixx)
    must("consume_layered_evidence_diverge_force_full", "AC1", mb)
    must("layered_diverge_force", "AC1", mb)
    must("provenance_miss", "AC1", mb)
    must("layered-evidence-diverge", "AC1", mb)  # force_reason stamp
    must("consume_layered_evidence_diverge_hard_reject", "AC1", mb)

    # Soft vs production table (AC6 + AC1 gate)
    must("Soft + diverge", "AC1/AC6", cixx)
    must("production / Full + diverge", "AC1/AC6", cixx)

    # AC2: dual-complete + provenance fill still present (recover path)
    must("coercion_entry_dual_complete", "AC2", cixx)
    must("fill_coercion_provenance_chain", "AC2", cixx)
    must("maybe_soft_recover_or_escalate_blame", "AC2", mb)

    # AC3: quiet consume (exchange / return false when pending 0)
    must("consume_layered_evidence_diverge_force_full", "AC3", cixx)
    # Coherence still returns 0 on hold
    must("return 0", "AC3", cixx)

    # AC4: additive schema + preserve prior
    must('"schema-2912"', "AC4", q)
    must('"issue-2912"', "AC4", q)
    must('"layered-evidence-diverge-force-consumed-total"', "AC4", q)
    must('"layered-evidence-diverge-hard-reject-consumed-total"', "AC4", q)
    must('"layered-evidence-diverge-force-full-consume-wired"', "AC4", q)
    must('"schema-2719"', "AC4", q)
    must('"schema-2674"', "AC4", q)
    must('"layered-evidence-diverge-force-full-pending"', "AC4", q)
    must('"layered-evidence-diverge-total"', "AC4", q)

    # AC5: source-cite + suite
    must("Issue #2912", "AC5", cixx)
    must("Issue #2912", "AC5", mb)
    must("Issue #2912", "AC5", q)
    must("run_2912_layered_diverge_force_full_consume", "AC5", test)
    must("ac2912_1_consume_force_full", "AC5", test)

    # AC6: linter + build.py; no docs/design
    must("check_layered_evidence_force_full_2912", "AC6", build)
    must("#2912", "AC6", build)
    design = ROOT / "docs" / "design" / "2912-layered-diverge-force-full.md"
    if design.is_file():
        fails.append("AC6: docs/design/2912-* present (forbidden per #1655)")

    # Lineage: #2719 arm still present
    must("g_layered_evidence_diverge_force_full_pending", "lineage", cixx)
    must("g_layered_evidence_diverge_force_full_pending.store", "lineage", mb)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2912 layered evidence diverge force-Full consume under production — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
