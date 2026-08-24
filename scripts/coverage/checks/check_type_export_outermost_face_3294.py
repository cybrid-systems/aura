#!/usr/bin/env python3
"""Issue #3294: query:type durable export requires an outermost success face.

Residual of #3203/#3237 under multi-round Agent canary: Soft TIMEOUT
"recovered" by a later local SOLVED (no outermost Full persist face)
must still refuse durable TypeIds. Evaluator tracks
`type_export_outermost_success_face_`: cleared on TIMEOUT/CONFLICT/
nested-inflight, re-armed only by the outermost grant helper. Soft
observe counter only; quiet SOLVED outermost zero extra.

Contract:
  AC1 Soft TIMEOUT → local SOLVED still refuses; no live TypeId
  AC2 Production TIMEOUT path unchanged (reject + clear)
  AC3 Outermost success + SOLVED still grants
  AC4 Soft observe counters only; zero extra on quiet SOLVED
  AC5 this linter + test_solve_delta_unresolved_export; no docs/design / invent

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

    ev = _read("src/compiler/evaluator.ixx")
    aud = _read("src/compiler/typed_mutation_audit.h")
    prim = _read("src/compiler/evaluator_primitives_eval.cpp")
    impl = _read("src/compiler/type_checker_impl.cpp")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    must("type_export_outermost_success_face_", "AC1 Evaluator flag", ev)
    must("!type_export_outermost_success_face_", "AC1 helper refuse", ev)
    must("if (!type_export_outermost_success_face_)", "AC1 copy_infer refuse", ev)
    must("g_type_export_soft_refuse_observe_total", "AC1 observe counter", aud)
    must("ac3294_1_soft_recovered_solved_refuses", "AC1 test", t)

    must("delta_timeout_fail_closed_total", "AC2 #3003", impl)
    must("delta_timeout_reject_total", "AC2 #2277", impl)
    must("production_defaults_active()", "AC2 production path", tc)
    must("clear_type_export_authority", "AC2 production clear", tc)
    must("ac3294_2_production_timeout_unchanged", "AC2 test", t)

    must("grant_type_export_authority", "AC3 persist grant", mb)
    must("type_export_outermost_success_face_ = true", "AC3 grant re-arms", ev)
    must("ac3294_3_outermost_grant_rearms", "AC3 test", t)

    must("Soft observe counter only", "AC4 observe-only doc", aud)
    must("ac3294_4_quiet_solved_zero_extra", "AC4 test", t)
    h = ev.find("bool type_export_is_authoritative() const noexcept")
    if h < 0:
        fails.append("AC4: Evaluator type_export_is_authoritative missing")
    else:
        end = ev.find("}", h)
        body = ev[h:end] if end > h else ""
        if "production_defaults_active" in body:
            fails.append("AC4: production_defaults_active on quiet helper")
        if "if (!last_type_solve_solved_)" not in body:
            fails.append("AC4: helper must consult last_type_solve_solved_ first")

    must("Issue #3294", "AC5 Evaluator cite", ev)
    must("Issue #3294", "AC5 audit cite", aud)
    must("Issue #3294", "AC5 prim cite", prim)
    must("no outermost success face", "AC5 annotate gate", prim)
    must("check_type_export_outermost_face_3294", "AC5 build.py", build)
    must("ac3294_5_source_and_linter", "AC5 test", t)

    if "g_3294_" in ev or "g_3294_" in aud:
        fails.append("AC5: invented g_3294_* counter")

    if (ROOT / "tests" / "compiler" / "test_issue_3294.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3294.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3294.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3294.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3294-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3294 type_export_outermost_face:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3294 type_export_outermost_face: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
