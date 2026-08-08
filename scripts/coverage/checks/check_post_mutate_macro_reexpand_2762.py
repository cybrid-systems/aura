#!/usr/bin/env python3
"""Issue #2762: post-mutate incremental macro re-expand under Guard cascade.

Refines closed #165/#2096: MutationBoundaryGuard success cascade must
invoke post_mutation_macro_reexpand (call-site splice + MacroIntroduced
restamp) so EDSL mutate:* → next eval-current sees fully hygienic AST.
Quiet path when macros_ empty (zero cost).

Contract (one row per AC):
  AC1 Guard cascade calls post_mutation_macro_reexpand on mutation log;
     reexpand splices via set_child; MacroDef body dirty → global call sites.
  AC2 Closed loop expand → mutate → re-expand → eval holds MacroIntroduced.
  AC3 Quiet non-macro path (macros_.empty early return).
  AC4 Concurrent fiber / steal contracts preserved (reuse existing restamp).
  AC5 Additive post_mutate_macro_reexpand_* metrics + schema-2762;
     #2037/#2038 surfaces preserved; no docs/design/*.
  AC6 Extend test_hygiene_mutate_closed_loop.cpp; this linter wired.

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    qe = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    # AC1 — cascade wire + splice + MacroDef body path.
    must("#2762", "AC1", mut)
    must("post_mutation_macro_reexpand", "AC1", mut)
    must("push_post_mutate_incremental_cascade", "AC1", mut)
    must("push_post_mutate_incremental_cascade", "AC1", emb)
    must("post_mutation_macro_reexpand", "AC1", efl)
    must("set_child", "AC1", efl)
    must("#2762", "AC1", efl)
    must("macros_body_dirty", "AC1", efl)

    # AC2 — restamp after reexpand.
    must("restamp_macro_introduced_generations", "AC2", efl)
    must("restamp_macro_introduced_subtree", "AC2", efl)

    # AC3 — quiet path.
    must("macros_.empty()", "AC3", efl)
    must("macros_.empty()", "AC3", mut)

    # AC4 — still under Guard / cascade (steal isolation elsewhere).
    must("MutationBoundaryGuard", "AC4", emb)

    # AC5 — metrics + query.
    must("post_mutate_macro_reexpand_total", "AC5", met)
    must("post_mutate_macro_reexpand_cascade_total", "AC5", met)
    must_key("post-mutate-macro-reexpand-total", "AC5", q + qe)
    must_key("schema-2762", "AC5", q + qe)
    must_key("issue-2762", "AC5", q + qe)
    must_key("schema-2037", "AC5", q)
    must_key("schema-2038", "AC5", qe)

    # AC6 — tests + linter + no docs/design.
    must("ac2762_1_cascade_wires_reexpand", "AC6", t)
    must("ac2762_2_closed_loop_expand_mutate_reexpand", "AC6", t)
    must("ac2762_3_quiet_non_macro_path", "AC6", t)
    must("ac2762_5_observability", "AC6", t)
    must("ac2762_6_source_and_linter", "AC6", t)
    must("check_post_mutate_macro_reexpand_2762", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2762.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2762.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2762-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2762 post-mutate macro re-expand under Guard cascade — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
