#!/usr/bin/env python3
"""Issue #3722 source-cite gate: rollback / rollback-since join the effect gate.

Evaluator host prims `rollback` / `rollback-since`
(src/compiler/evaluator_primitives_mutation.cpp) called FlatAST::rollback /
rollback_since with NO require_effect, NO isolation consult, NO audit join —
a Restricted+MT tenant could structurally undo a foreign mutation from a
mid learned via mutation-history (no tenant filter).

ACs:
  AC1  the rollback lambda resolves mid → target node and gates through
       require_effect_for_node_id (kEffectMutate, "rollback") BEFORE the
       FlatAST write; the lambda cites #3722; deny returns make_bool(false).
  AC2  the rollback-since lambda gates EVERY committed record in the
       revert set (deny-first loop) BEFORE rollback_since(; deny returns
       make_int(0) (zero topology change).
  AC3  "rollback"/"rollback-since" stay out of
       check_side_effect_node_id_mandate_2942.py; its EXEMPT_2ARG_OPS
       count guard stays intact (count owned by #2942; waves may grow it).
  AC4  Soft/Off unchanged: no sandbox-mode branching inside the two gated
       lambda bodies — the require_effect zero-cost short-circuit is the
       only Soft/Off face.
  AC5  tests cite #3722 (runtime ACs in test_tenant_isolation_enforcement);
       no test_issue_3722.cpp; no docs/design/3722-*.
  AC6  build.py wires this linter; no new query key in the gated region.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRIM = ROOT / "src" / "compiler" / "evaluator_primitives_mutation.cpp"
TST = ROOT / "tests" / "core" / "test_tenant_isolation_enforcement.cpp"
NODEID_LINT = ROOT / "scripts" / "coverage" / "checks" / "check_side_effect_node_id_mandate_2942.py"


def _lambda_body(text: str, start_marker: str) -> str:
    """Return a bounded slice of the lambda body starting at start_marker."""
    pos = text.find(start_marker)
    if pos < 0:
        return ""
    end = text.find("add(", pos + len(start_marker))
    return text[pos : end if end > 0 else len(text)]


def main() -> int:
    prim = PRIM.read_text() if PRIM.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    nodeid_lint = NODEID_LINT.read_text() if NODEID_LINT.exists() else ""
    build = (ROOT / "build.py").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    rollback_body = _lambda_body(prim, 'add("rollback",')
    rsince_body = _lambda_body(prim, 'add("rollback-since",')

    good = (
        "#3722" in rollback_body
        and 'require_effect_for_node_id(kEffectMutate, "rollback"' in rollback_body
        and rollback_body.find('require_effect_for_node_id(kEffectMutate, "rollback"')
        < rollback_body.find("rollback(mid)")
        and "return make_bool(false);" in rollback_body
    )
    report("AC1", good, "rollback gate precedes FlatAST write; deny → #f; cites #3722")

    good = (
        "#3722" in rsince_body
        and 'require_effect_for_node_id(kEffectMutate, "rollback-since"' in rsince_body
        and "MutationStatus::Committed" in rsince_body
        and rsince_body.find('require_effect_for_node_id(kEffectMutate, "rollback-since"')
        < rsince_body.find("rollback_since(")
        and "return make_int(0);" in rsince_body
    )
    report("AC2", good, "rollback-since deny-first revert-set gate before write; deny → 0")

    good = (
        '"rollback"' not in nodeid_lint
        and "rollback-since" not in nodeid_lint
        and "len(EXEMPT_2ARG_OPS) !=" in nodeid_lint
    )
    report("AC3", good, "no rollback entries in EXEMPT_2ARG_OPS (#2942 count guard intact)")

    gated = rollback_body + rsince_body
    good = (
        "effect_sandbox_mode" not in gated
        and "SandboxMode::" not in gated
        and 'require_effect_on_ref(kEffectMutate, "rollback"' in gated
    )
    report("AC4", good, "no sandbox branching in gated lambdas (Soft/Off zero-cost face)")

    good = (
        "#3722 AC1" in tst
        and "#3722 AC2" in tst
        and "#3722 AC3" in tst
        and "#3722 AC4" in tst
        and "#3722 AC5" in tst
        and "#3722 AC6" in tst
        and not (ROOT / "tests" / "core" / "test_issue_3722.cpp").exists()
        and not (ROOT / "docs" / "design" / "3722-rollback-effect-gate.md").exists()
    )
    report("AC5", good, "runtime ACs live in test_tenant_isolation_enforcement; no issue-file/doc")

    good = (
        "check_rollback_effect_gate_3722.py" in build
        and 'insert_kv("rollback-gate' not in prim
        and "query:rollback-gate" not in prim
    )
    report("AC6", good, "build.py wires the linter; no new query key")

    print(f"check_rollback_effect_gate_3722: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
