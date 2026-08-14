#!/usr/bin/env python3
"""Issue #2998: residual silent sentinels on core primitives.

AC:
  1. list member/take/drop/filter/reverse/list-ref true errors
  2. math modulo/abs/inexact->exact coerce/arity errors
  3. json-parse type/invalid errors; json-get-string type mismatch
  4. car/cdr corrupted pair + vector-length / vector->list
  5. predicates stay boolean; documented empty/not-found stay silent
  6. convention doc + existing 2914 suite + build.py
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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    lst = _read("src/compiler/evaluator_primitives_list.cpp")
    must("2998" in lst, "AC1: list.cpp cites #2998")
    must("member: not a list" in lst, "AC1: member type error")
    must("member: improper list" in lst, "AC1: member improper error")
    must("documented not-found" in lst, "AC5: member not-found documented")
    must("take: not a list" in lst, "AC1: take type error")
    must("drop: not a list" in lst, "AC1: drop type error")
    must("drop past end" in lst, "AC5: drop past end empty")
    must("filter: not a list" in lst, "AC1: filter type error")
    must("reverse: not a list" in lst, "AC1: reverse type error")
    must("list-ref: index must be a non-negative integer" in lst, "AC1: list-ref index type")
    after = lst.split('"list?"', 1)
    if len(after) > 1:
        body = after[1][:1200]
        must("make_bool(false)" in body, "AC4: list? stays boolean")
        must("make_primitive_error" not in body, "AC4: list? is not an error path")

    math = _read("src/compiler/evaluator_primitives_math.cpp")
    must("try_coerce_int" in math, "AC2: math coerce helper")
    must("modulo: expected number" in math, "AC2: modulo non-numeric")
    must("modulo: too few args" in math, "AC2: modulo arity")
    must("abs: expected number" in math, "AC2: abs coerce")
    must("inexact->exact: expected number" in math, "AC2: inexact->exact type")

    js = _read("src/compiler/evaluator_primitives_json.cpp")
    must("json-parse: expected a string" in js, "AC3: json-parse type")
    must("json-parse: invalid input" in js, "AC3: json-parse invalid token")
    must("json-get-string: expected two strings" in js, "AC3: json-get-string type")

    pair = _read("src/compiler/evaluator_primitives_pair.cpp")
    must("car: corrupted pair" in pair, "AC4: car corrupted")
    must("cdr: corrupted pair" in pair, "AC4: cdr corrupted")

    vec = _read("src/compiler/evaluator_primitives_vector.cpp")
    must("vector-length: not a vector" in vec, "AC4: vector-length type")
    must("vector->list: not a vector" in vec, "AC4: vector->list type")
    after_v = vec.split('"vector?"', 1)
    if len(after_v) > 1:
        body = after_v[1][:600]
        must("make_bool" in body, "AC4: vector? stays boolean")
        must("make_primitive_error" not in body, "AC4: vector? is not an error path")

    doc = _read("docs/stdlib/primitive-error-convention.md")
    must("2998" in doc, "AC6: convention doc cites #2998")
    must("member" in doc and "not-found" in doc, "AC6: member not-found rule")
    must("cadr" in doc, "AC6: cadr family stays soft")

    suite = _read("tests/suite/query_primitives_split_2914.aura")
    must("FAIL-2998" in suite, "AC6: 2914 suite extended")
    must("member not-found stays 0" in suite, "AC5: suite keeps not-found")
    must("json-parse invalid" in suite, "AC6: suite json invalid")

    build = _read("build.py")
    must("prim_error_convention_2998" in build or "prim-error-convention-2998" in build, "AC6: build.py gate")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2998 primitive error-return residuals — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
