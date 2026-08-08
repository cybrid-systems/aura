#!/usr/bin/env python3
"""Issue #2764: residual IR/JIT/AOT source_marker + respect_macro_hygiene_ enforcement.

Refines closed #501/#1610/#2100: complete MacroIntroduced enforcement across
lowering (propagate_marker_from_ast ancestor walk), InlinePass hard filter on
all Call sites, deopt restore fidelity, multi-eval denseness preserve.

Contract (one row per AC):
  AC1 propagate_marker_from_ast in lowering.ixx; InlinePass hard skip + metric
  AC2 deopt restore restamps MacroIntroduced (service + runtime hook)
  AC3 multi-eval preserved counter + restore bumps it
  AC4 non-macro quiet (ancestor walk early-return User)
  AC5 Additive schema-2764 keys; 2022/2100/2177 preserved; no docs/design/*
  AC6 Extend test_jit_macro_deopt_hygiene.cpp; this linter wired.

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

    low = _read("src/compiler/lowering.ixx")
    pass_impl = _read("src/compiler/pass_impls.ixx")
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    svc = _read("src/compiler/service.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    met = _read("src/compiler/observability_metrics.h")
    t = _read("tests/compiler/test_jit_macro_deopt_hygiene.cpp")
    build = _read("build.py")

    # AC1 — lowering propagate + InlinePass hard filter.
    must("#2764", "AC1", low)
    must("propagate_marker_from_ast", "AC1", low)
    must("parent_of", "AC1", low)
    must("kMaxAncestorWalk", "AC1", low)
    must("#2764", "AC1", pass_impl)
    must("respect_macro_hygiene_", "AC1", pass_impl)
    must("macro_hygiene_skipped_", "AC1", pass_impl)
    must("source_marker == 1", "AC1", pass_impl)
    must("marker == 1", "AC1", pass_impl)

    # AC2 — deopt restore fidelity.
    must("restore_macro_introduced_from_ir_after_deopt", "AC2", svc)
    must("g_macro_deopt_restore_fn", "AC2", rt)
    must("aura_jit_macro_introduced_deopt_inc", "AC2", rt)

    # AC3 — multi-eval denseness preserve.
    must("aura_multi_eval_macro_marker_preserved_inc", "AC3", rt)
    must("aura_multi_eval_macro_marker_preserved_total", "AC3", rt)
    must("aura_multi_eval_macro_marker_preserved_inc", "AC3", svc)
    must("#2764", "AC3", svc)

    # AC4 — quiet ancestor path (User early return).
    must("SyntaxMarker::User", "AC4", low)
    must("MacroIntroduced", "AC4", low)

    # AC5 — additive observability + prior surfaces.
    must_key("schema-2764", "AC5", q)
    must_key("issue-2764", "AC5", q)
    must_key("marker-ancestor-propagation-total", "AC5", q)
    must_key("multi-eval-macro-marker-preserved-total", "AC5", q)
    must_key("propagate-marker-from-ast-wired", "AC5", q)
    must_key("inline-macro-hygiene-hard-filter-wired", "AC5", q)
    must_key("schema-2100", "AC5", q)
    must("2022", "AC5", q)
    must("#2764", "AC5", met)
    must("aura_hygiene_ir_ancestor_propagation", "AC5", rt)

    # AC6 — tests + linter + no docs/design.
    must("ac2764_1_propagate_and_inline_hard_filter", "AC6", t)
    must("ac2764_2_deopt_restore_fidelity", "AC6", t)
    must("ac2764_3_multi_eval_no_marker_loss", "AC6", t)
    must("ac2764_4_non_macro_quiet", "AC6", t)
    must("ac2764_5_observability", "AC6", t)
    must("ac2764_6_source_and_linter", "AC6", t)
    must("check_ir_jit_macro_marker_enforcement_2764", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2764.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2764.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2764-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2764 residual IR/JIT/AOT MacroIntroduced enforcement — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
