#!/usr/bin/env python3
"""Issue #2992: non-strict ground-type consistency Agent feedback.

consistent_unify still returns true for two grounds (program continues).
Balanced default emits Warning on Int~String; Dynamic ~ T and Int↔Float
stay quiet. Knob: AURA_GRADUAL_PERMISSIVENESS=permissive|balanced|strict.

Contract:
  AC1 Int vs String → Warning in default non-strict (balanced)
  AC2 Dynamic ~ T stays fully permissive
  AC3 Int ↔ Float stays quiet
  AC4 AURA_GRADUAL_PERMISSIVENESS knob + type:set-gradual-permissiveness
      + schema-2992
  AC5 extend existing bidirectional suite; no docs/design / invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    tix = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    compile_p = _read("src/compiler/evaluator_primitives_compile.cpp")
    svc = _read("src/compiler/service.ixx")
    q = read_query_prims()
    ta = _read("tests/compiler/test_bidirectional_annotation.cpp")
    ts = _read("tests/core/test_bidirectional_stats.cpp")
    build = _read("build.py")

    # AC1 — Warning on incompatible grounds; unify boolean stays true
    must("maybe_report_ground_inconsistency", "AC1", impl)
    must("incompatible ground types", "AC1", impl)
    must("ErrorKind::Warning", "AC1", impl)
    must("do NOT flip this boolean", "AC1 unify stays true", impl)
    must("ac2992_1_int_string_warning", "AC1", ta)

    # AC2 — Dynamic stays permissive
    must("Dynamic ~ T stays fully permissive", "AC2", tix)
    must("reg_.dynamic_type()", "AC2 skip Dynamic", impl)
    must("ac2992_2_dynamic_permissive", "AC2", ta)

    # AC3 — numeric allow-list
    must("TypeTag::INT && to_tag == TypeTag::FLOAT", "AC3", impl)
    must("ac2992_3_numeric_quiet", "AC3", ta)

    # AC4 — knob + docs-in-code + EDSL + schema
    must("AURA_GRADUAL_PERMISSIVENESS", "AC4", tix)
    must("GradualPermissiveness", "AC4", tix)
    must("parse_gradual_permissiveness", "AC4", tix)
    must("type:set-gradual-permissiveness", "AC4", compile_p)
    must_key("schema-2992", "AC4 compile stats", compile_p)
    must_key("schema-2992", "AC4 query stats", q)
    must("set_gradual_permissiveness", "AC4", svc)
    must("gradual_ground_incompatible_warning_total", "AC4", met)
    must("ac2992_5_schema", "AC4", ts)
    must("ac2992_7_setter", "AC4", ts)

    # AC5 — extend existing suite; no invent / no docs/design
    must("ac2992_4_permissive_silent", "AC5", ta)
    must("ac2992_6_eval_continues", "AC5", ts)
    must("check_gradual_permissiveness_2992", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2992.cpp").is_file():
        fails.append("AC5: test_issue_2992.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2992-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2992 gradual permissiveness — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
