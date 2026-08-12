#!/usr/bin/env python3
"""Issue #2922: extract ast_unparse + optional pretty-print.

AC:
  1. Module aura.core.ast_unparse with unparse_to_string / UnparseOptions
  2. current-source is thin wrapper (no inline recursive unparse lambda)
  3. snapshot uses library (no current-source lookup for workspace unparse)
  4. pretty + define_fn_sugar options
  5. Unit test without Evaluator + suite smoke + cmake/build.py + docs
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

    mod = _read("src/core/ast_unparse.ixx")
    must("export module aura.core.ast_unparse" in mod, "AC1: module")
    must("unparse_to_string" in mod, "AC1: unparse_to_string")
    must("UnparseOptions" in mod, "AC1: UnparseOptions")
    must("pretty" in mod and "max_depth" in mod, "AC4: options fields")
    must("define_fn_sugar" in mod, "AC4: define_fn_sugar")
    must("Evaluator" not in mod, "AC1: no Evaluator in unparse module")
    must(
        "operator+" not in mod or "no recursive" in mod.lower() or "reserve" in mod,
        "AC1: non-quadratic allocation note/reserve",
    )

    modules = _read("cmake/AuraModules.cmake")
    must("ast_unparse.ixx" in modules, "AC1: AuraModules lists ixx")

    core = _read("src/core/core.ixx")
    must("ast_unparse" in core, "AC1: core re-exports")

    eval_p = _read("src/compiler/evaluator_primitives_eval.cpp")
    must("unparse_to_string" in eval_p, "AC2: current-source uses library")
    must("kMaxUnparseDepth" not in eval_p, "AC2: inline unparse removed")
    must(":pretty" in eval_p, "AC4: :pretty keyword")
    must(":define-fn-sugar" in eval_p, "AC4: :define-fn-sugar keyword")

    ast_p = _read("src/compiler/evaluator_primitives_ast.cpp")
    must("unparse_to_string" in ast_p, "AC3: snapshot path uses library")
    # workspace_source_string body must not lookup current-source
    idx = ast_p.find("workspace_source_string")
    must(idx >= 0, "AC3: workspace_source_string present")
    if idx >= 0:
        window = ast_p[idx : idx + 600]
        must('lookup("current-source")' not in window, "AC3: no primitive re-entry")

    svc = _read("src/compiler/service.ixx")
    must("unparse_to_string" in svc, "AC3: service get_workspace_source_fn uses library")

    unit = _read("tests/compiler/test_ast_unparse.cpp")
    must("2922" in unit and "unparse_to_string" in unit, "AC5: unit test")
    must("import aura.core.ast_unparse" in unit, "AC5: imports unparse module")
    must(
        "import aura.compiler.evaluator" not in unit and "CompilerService" not in unit,
        "AC5: unit without Evaluator/CompilerService",
    )

    suite = _read("tests/suite/ast_unparse_pretty_2922.aura")
    must("2922" in suite and ":pretty" in suite, "AC5: suite pretty smoke")

    cmake = _read("CMakeLists.txt")
    must("test_ast_unparse" in cmake, "AC5: cmake")

    build = _read("build.py")
    must("ast-unparse-2922" in build or "ast_unparse_2922" in build, "AC5: build.py")

    doc = _read("docs/stdlib/ast-unparse.md")
    must("2922" in doc and "unparse_to_string" in doc, "AC5: docs")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2922 ast_unparse extract — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
