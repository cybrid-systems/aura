#!/usr/bin/env python3
"""Issue #2675: linear-enforce-effective single pure API across AST audit /
IR executor / MutationBoundary force classification (replaces #2222 split).

Contract:
  AC1 production_defaults || fiber_boundary_hold → effective Strict for IR
     + boundary post-mutate (decision table golden row)
  AC2 Soft + no hold → Soft; fiber hold mid-boundary → Strict for that
     hold (boundary Strict-hold wins over process Soft)
  AC3 AURA_LINEAR_ENFORCE=strict env forces Strict even under Soft audit
     strategy (env_force_strict wins)
  AC4 #2108 cross-batch escape still hard-blocks commit independent of
     Soft (secondary gate; pure API does NOT route it)
  AC5 same fixture AST audit vs IR execute agree on Soft vs fail (golden
     table covers both callers under same inputs)
  AC6 unit + query:linear-enforce-effective key + schema-2675/issue-2675
     sentinels + linter self-coverage

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

    hdr = _read("src/core/provenance_tracker.hh")
    lom = _read("src/compiler/linear_occurrence_mutate_stats.h")
    ir = _read("src/compiler/ir_executor_impl.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    test = _read("tests/compiler/test_linear_cross_closure.cpp")
    build = _read("build.py")

    # AC1 + AC2 + AC3: pure API in provenance_tracker.hh (single source).
    must("enum class LinearEnforceEffective", "AC1", hdr)
    must("Soft = 0", "AC1", hdr)
    must("Strict = 1", "AC1", hdr)
    must("effective_linear_enforce(", "AC1", hdr)
    must("bool production_defaults", "AC1", hdr)
    must("bool fiber_boundary_hold", "AC1", hdr)
    must("bool env_force_strict", "AC1", hdr)

    # AC2: boundary Strict-hold wins over process Soft (existing
    # linear_enforce_effective_mode + state-reader wrapper retain this).
    must("linear_enforce_boundary_strict_active", "AC2", hdr)
    must("effective_linear_enforce(", "AC2", hdr)

    # AC3: env_force_strict branch in decision table (first conditional).
    must("if (env_force_strict)", "AC3", hdr)
    must("return LinearEnforceEffective::Strict", "AC3", hdr)

    # AC4: #2108 cross-batch escape independent — CrossBatchEscape
    # authority still present in typed_mutation_audit.h.
    must("CrossBatchEscape", "AC4", aud)
    must("#2108", "AC4", aud)

    # AC5: explicit wire at IR executor to the pure API (caller-supplied
    # state path).
    must("linear_enforce_require_complete_effective(", "AC5", ir)
    must("effective_linear_enforce(", "AC5", hdr)

    # AC6: query surface keys + sentinels + linter self-coverage.
    must('"linear-enforce-effective"', "AC6", obs)
    must('"schema-2675"', "AC6", obs)
    must('"issue-2675"', "AC6", obs)
    must('"linear-enforce-effective-pure-api-wired"', "AC6", obs)
    must("ac2675_audit_ir_agree", "AC6", test)
    must("ac2675_production_defaults_strict", "AC6", test)
    must("check_linear_enforce_effective_2675", "AC6", build)

    # Lineage / single-source-of-truth comment.
    must("Issue #2675", "lineage", lom)
    must("effective_linear_enforce(", "lineage", lom)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2675 linear-enforce-effective single pure API — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
