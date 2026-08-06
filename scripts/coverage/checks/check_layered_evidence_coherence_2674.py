#!/usr/bin/env python3
"""Issue #2674: layered dead-coercion evidence-coherence production gate
(refine #2645 — was test/linter-only; #2674 adds production-path
consistency check + Agent-visible query surface).

Contract:
  AC1 evidence != 0 → ast-elided++ AND meta stamp with mid + evidence + type_tag
     (already covered by #2645; #2674 adds g_dead_coercion_ast_elided_with_evidence_total
      counter bumped at both elision sites)
  AC2 evidence == 0 → AST may elide; NO meta stamp (zero cost)
     (covered by #2645)
  AC3 IR pass elision counts visible on layered stats
     (covered by #2645)
  AC4 Soft empty cone / no evidence → zero meta / zero forced work
     (covered by #2644/#2645)
  AC5 (NEW) diverge sample counter + query keys (schema-2674 / issue-2674 /
     ast-elided-with-evidence / layered-evidence-diverge-total /
     layered-evidence-coherence-wired); Soft observe / Full optional escalate
  AC6 (NEW) check_layered_evidence_coherence() called at MutationBoundaryGuard
     Phase 5 outermost exit (coarse boundary placement, zero cost on hot path)

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
    _read("src/compiler/optimization_passes.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_dead_coercion_layered.cpp")
    build = _read("build.py")
    dce = _read("src/compiler/dce_elided_deopt_meta.h")

    # AC1: new counter present + bumped at both elision sites (>= 2)
    must("g_dead_coercion_ast_elided_with_evidence_total", "AC1", cixx)
    bump_count = cixx.count("g_dead_coercion_ast_elided_with_evidence_total.fetch_add")
    if bump_count < 2:
        fails.append(f"AC1: counter bumped {bump_count} time(s) (need >= 2 — identity + Dynamic-tag elision sites)")

    # AC2: diverge counter present + exposed via query surface (no hard-reject
    # in mb.cpp — observability first per body AC5)
    must("g_layered_evidence_diverge_total", "AC2", cixx)
    must("layered-evidence-diverge-total", "AC2", q)
    if "g_layered_evidence_diverge_total" in mb:
        fails.append("AC2: diverge counter NOT expected in mb.cpp (observability-only)")

    # AC3: check function exported + invariant reads all 3 counters
    must("export inline void", "AC3", cixx)
    # Function name may live on next line after clang-format wraps the
    # declaration (export inline void\\ncheck_layered_evidence_coherence).
    # Split the check so format-driven wrapping doesn't break the linter.
    must("check_layered_evidence_coherence(std::uint64_t", "AC3", cixx)
    must("g_dead_coercion_ast_elided_with_evidence_total", "AC3", cixx)
    must("dead_coercion_ir_narrow_evidence_hits", "AC3", cixx)
    must("dce_deopt_meta_stamped_total", "AC3", cixx)
    must("g_layered_evidence_diverge_total.fetch_add", "AC3", cixx)

    # AC4: function called at MutationBoundaryGuard Phase 5 exit
    must("check_layered_evidence_coherence(", "AC4", mb)
    must("#2674", "AC4", mb)  # source-cite
    must("MutationBoundary outermost exit", "AC4", cixx)

    # AC5: query surface has #2674 keys
    must('"schema-2674"', "AC5", q)
    must('"issue-2674"', "AC5", q)
    must('"ast-elided-with-evidence"', "AC5", q)
    must('"layered-evidence-diverge-total"', "AC5", q)
    must('"layered-evidence-coherence-wired"', "AC5", q)
    must("FlatHashTable::create(256)", "AC5", q)  # capacity bump

    # AC6: test file + build.py wired
    must("run_2674_layered_coherence", "AC6", test)
    must("ac2674_linter_and_build_py", "AC6", test)
    must("check_layered_evidence_coherence_2674", "AC6", build)
    must("#2674", "AC6", cixx)  # source-cite in production

    # Source-cite lineage to #2645 / #2611 / #2624 (additive, not replacement)
    must("#2645", "lineage", cixx)
    must("#2611", "lineage", dce)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2674 layered dead-coercion evidence-coherence production gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
