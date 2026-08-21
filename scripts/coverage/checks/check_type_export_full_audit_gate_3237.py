#!/usr/bin/env python3
"""Issue #3237: query:type gates on Full audit + no residual dirty/TIMEOUT.

#3004 persist grant + #3031 drain + #3203 TIMEOUT refuse still left a
window: query:type / type_export_is_authoritative could hand Agent a
type computed under Soft observation or before the Full residual face
closed (concurrent densify / nested abort). Production refuses residual;
Soft unchanged; quiet two loads. Reuses force_reason 16. No new query key.

Contract:
  AC1 Production + residual dirty/TIMEOUT → not-authoritative; no green proof
  AC2 Soft unchanged; quiet residual-clear is one face load
  AC3 lineage #3004/#3031/#3203/#3225
  AC4 extend test_solve_delta_unresolved_export; this linter; no invent / docs

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ixx = _read("src/compiler/type_checker.ixx")
    ev = _read("src/compiler/evaluator.ixx")
    aud = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    prim = _read("src/compiler/evaluator_primitives_eval.cpp")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )
    build = _read("build.py")

    must("kTypeExportFullAuditGateIssue = 3237", "AC1 stamp", ixx)
    must("type_export_residual_faces_clear", "AC1 helper", aud)
    must("type_export_residual_faces_clear", "AC1 Evaluator", ev)
    must("pending_full_solve_residual_face_hit", "AC1 persist skip", mb)
    must("force_reason=*/16", "AC1 reused force_reason", mb)
    must("ac3237_1_production_residual_not_authoritative", "AC1 test", t)

    must("ac3237_2_soft_quiet", "AC2 test", t)
    must("type_export_residual_faces_clear", "AC2 helper", aud)
    h = ev.find("bool type_export_is_authoritative() const noexcept")
    if h < 0:
        fails.append("AC2: Evaluator type_export_is_authoritative missing")
    else:
        end = ev.find("}", h)
        body = ev[h:end] if end > h else ""
        if "production_defaults_active" in body:
            fails.append("AC2: production_defaults_active on quiet helper")
        if "if (!last_type_solve_solved_)" not in body:
            fails.append("AC2: helper must consult last_type_solve_solved_ first")

    must("grant_type_export_authority", "AC3 #3004 persist", mb)
    must("drain_pending_full_solve_before_commit", "AC3 #3031", mb)
    must("if (!last_type_solve_solved_", "AC3 #3203 grant", ev)
    must("Issue #3225", "AC3 #3225 seqlock", mb)
    must("ac3237_3_lineage", "AC3 test", t)
    if "schema-3237" in q:
        fails.append("AC3: new schema-3237 query key")
    if "g_3237_" in ev or "g_3237_" in aud:
        fails.append("AC3: new g_3237_* counter")

    must("check_type_export_full_audit_gate_3237", "AC4 build.py", build)
    must("ac3237_4_source_linter", "AC4 test", t)
    must("Issue #3237", "AC4 prim", prim)
    must("Issue #3237", "AC4 typecheck", tc)
    if (ROOT / "tests" / "compiler" / "test_issue_3237.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3237.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3237.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3237.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3237-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3237 type_export_full_audit_gate:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3237 type_export_full_audit_gate: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
