#!/usr/bin/env python3
"""Issue #2979: outermost Phase-5 consume of layered-evidence force-Full pending.

#2912 consume in exit_mutation_boundary could be stolen by nested exits
or dropped by Sampled recover. Phase-5 outermost (success +
non-intentional-failure) consumes pending and runs one Full invariant
sample. Soft observe-only. Quiet: one exchange.

Contract:
  AC1 Production/Full + pending → Phase-5 consume + Full sample
  AC2 Soft/Sampled: no force-Full from this channel
  AC3 Quiet pending==0: consume no-op
  AC4 HARD consume stamps force_reason; default not commit reject
  AC5 Additive schema-2979; #2674/#2912 preserved
  AC6 Source-cite + suite; no docs/design/; linter + build.py

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
    q = _read("src/compiler/evaluator_primitives_query_type_stats.cpp")
    tl = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    test = _read("tests/compiler/test_dead_coercion_layered.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #2979", "AC1", mb)
    must("consume_layered_evidence_diverge_force_full", "AC1", mb)
    must("note_layered_evidence_diverge_force_full_sample", "AC1", mb)
    must("ensure_mutation_invariants", "AC1", mb)
    must("partial_recovery_2979", "AC1", mb)
    must("!nested_boundary", "AC1 nested", mb)
    must("g_layered_evidence_diverge_force_full_sample_total", "AC1", cixx)
    must("ac2979_1_phase5_consume_sample", "AC1", test)

    # AC2
    must("Soft + diverge", "AC2", cixx)
    must("layered_diverge_delta > 0", "AC2", mb)
    must("ac2979_2_soft_observe_only", "AC2", test)

    # AC3
    must("one exchange", "AC3", mb)
    must("ac2979_3_quiet_zero_cost", "AC3", test)

    # AC4
    must("consume_layered_evidence_diverge_hard_reject", "AC4", mb)
    must("layered-evidence-diverge", "AC4", mb)
    must("not hard-reject of the current commit", "AC4", mb)
    must("AURA_LAYERED_COERCION_DIVERGE_HARD", "AC4", cixx)
    must("ac2979_4_hard_force_reason", "AC4", test)

    # AC5
    must('"schema-2979"', "AC5", q)
    must('"issue-2979"', "AC5", q)
    must('"layered-evidence-diverge-force-full-sample-total"', "AC5", q)
    must('"layered-evidence-diverge-phase5-consume-wired"', "AC5", q)
    must('"schema-2912"', "AC5", q)
    must('"schema-2674"', "AC5", q)
    must('"schema-2979"', "AC5 health", tl)
    must("ac2979_5_additive_schema", "AC5", test)

    # AC6
    must("Issue #2979", "AC6", cixx)
    must("Issue #2979", "AC6", mb)
    must("run_2979_phase5_consume_force_full_sample", "AC6", test)
    must("check_layered_evidence_phase5_consume_2979", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2979.cpp").is_file():
        fails.append("AC6: test_issue_2979.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2979*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2979 Phase-5 consume + Full sample — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
