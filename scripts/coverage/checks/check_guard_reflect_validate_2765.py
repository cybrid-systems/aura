#!/usr/bin/env python3
"""Issue #2765: Guard success-path reflect auto_validate / hygiene_validate.

Integrates post_mutation_reflect_validate into outermost MutationBoundaryGuard
success path with Agent-facing counters, optional flag (default ON), Soft
metric-only fail, and Strict force-rollback. Refines #488/#596/#1611.

Contract (one row per AC):
  AC1 exit success path calls post_mutation_reflect_validate + metrics;
     Strict fail → structural rollback
  AC2 Happy-path Guard mutate bumps guard_reflect_validate_total
  AC3 Flag off → skip (zero validate cost)
  AC4 MacroIntroduced / MutationReflectHealth integration preserved
  AC5 Additive schema-2765 keys on query:guard-panic-reflect-stats;
     schema 596 preserved; no docs/design/*
  AC6 Extend test_guard_panic_reflect_fiber_resume_task6.cpp; this linter wired.

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    eix = _read("src/compiler/evaluator.ixx")
    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    met = _read("src/compiler/observability_metrics.h")
    t = _read("tests/serve/test_guard_panic_reflect_fiber_resume_task6.cpp")
    build = _read("build.py")

    # AC1 — Guard success wire + Strict rollback.
    must("#2765", "AC1", emb)
    must("post_mutation_reflect_validate", "AC1", emb)
    must("bump_guard_reflect_validate", "AC1", emb)
    must("bump_guard_reflect_validate_fail", "AC1", emb)
    must("bump_guard_reflect_validate_strict_rollback", "AC1", emb)
    must("is_strict", "AC1", emb)
    must("guard-reflect-validate-force-rollback", "AC1", emb)
    must("post_mutation_reflect_validate", "AC1", efl)

    # AC2 — counters + default enabled.
    must("guard_reflect_validate_total", "AC2", met)
    must("guard_reflect_validate_enabled_", "AC2", eix)
    must("get_guard_reflect_validate_enabled", "AC2", eix)
    must("set_guard_reflect_validate_enabled", "AC2", eix)

    # AC3 — skip path.
    must("bump_guard_reflect_validate_skipped", "AC3", emb)
    must("guard_reflect_validate_skipped_total", "AC3", met)

    # AC4 — hygiene / MacroIntroduced integration.
    must("MutationReflectHealth", "AC4", efl)
    must("MacroIntroduced", "AC4", efl)

    # AC5 — query keys + prior surface.
    must_key("schema-2765", "AC5", q)
    must_key("issue-2765", "AC5", q)
    must_key("guard-reflect-validate-total", "AC5", q)
    must_key("guard-reflect-validate-fail-total", "AC5", q)
    must_key("guard-reflect-validate-strict-rollback-total", "AC5", q)
    must_key("guard-reflect-validate-skipped-total", "AC5", q)
    must_key("guard-reflect-last-ok", "AC5", q)
    must("596", "AC5", q)  # schema lineage

    # AC6 — tests + linter.
    must("ac2765_1_guard_success_wires_validate", "AC6", t)
    must("ac2765_2_happy_path_bumps_total", "AC6", t)
    must("ac2765_3_flag_off_skips", "AC6", t)
    must("ac2765_4_macro_provenance_integration", "AC6", t)
    must("ac2765_5_observability", "AC6", t)
    must("ac2765_6_source_and_linter", "AC6", t)
    must("check_guard_reflect_validate_2765", "AC6", build)
    if (ROOT / "tests" / "serve" / "test_issue_2765.cpp").is_file():
        fails.append("AC6: tests/serve/test_issue_2765.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2765-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2765 Guard success-path reflect auto_validate / hygiene_validate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
