#!/usr/bin/env python3
"""Issue #2919: current-source unparse P0 tags + string/lambda fixes.

AC:
  1. Unparse arms for TypeAnnotation, Coercion, DefineType, Linear family
  2. Lambda dotted rest (int_value / HasLambdaDotted)
  3. String escapes for newline/tab/quote/backslash
  4. No P0 tag left only in default <N> path (arms present)
  5. Suite + build.py wiring

Issue #2922: unparse lives in src/core/ast_unparse.ixx; current-source is
a thin wrapper that calls unparse_to_string.
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

    # #2922: library is SSOT; eval primitive must still wire #2919 via call.
    lib = _read("src/core/ast_unparse.ixx")
    eval_src = _read("src/compiler/evaluator_primitives_eval.cpp")
    must(
        "Issue #2919" in lib or "Issue #2919" in eval_src,
        "AC: cites #2919",
    )
    must("unparse_to_string" in eval_src, "AC: current-source uses unparse_to_string")
    must("kCurrentSource" in eval_src, "missing kCurrentSource registration")

    chunk = lib if lib else eval_src
    for tag in (
        "NodeTag::TypeAnnotation",
        "NodeTag::Coercion",
        "NodeTag::DefineType",
        "NodeTag::Linear",
        "NodeTag::Move",
        "NodeTag::Borrow",
        "NodeTag::MutBorrow",
        "NodeTag::Drop",
    ):
        must(tag in chunk, f"AC1: unparse arm missing {tag}")

    must(
        "dotted" in chunk.lower()
        or "HasLambdaDotted" in chunk
        or ("int_value != 0" in chunk and "lambda" in chunk.lower()),
        "AC2: lambda dotted rest",
    )
    must(
        "escape_string" in chunk or "escape_string_literal" in chunk,
        "AC3: string escape helper",
    )
    must("case '\\n'" in chunk or "\\n" in chunk, "AC3: newline escape arm")
    must("case '\\t'" in chunk or "\\t" in chunk, "AC3: tab escape arm")
    must("case '\"'" in chunk or '\\"' in chunk, "AC3: quote escape arm")

    suite = _read("tests/suite/current_source_unparse_2919.aura")
    must("2919" in suite and "roundtrip" in suite, "AC5: suite present")
    must("Type" in suite or "define-type" in suite, "AC5: suite covers types")
    must("lambda" in suite and "Linear" in suite, "AC5: lambda + Linear")

    build = _read("build.py")
    must(
        "current-source-unparse-2919" in build or "current_source_unparse_2919" in build,
        "AC5: build.py gate",
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2919 current-source unparse P0 tags — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
