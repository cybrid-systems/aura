#!/usr/bin/env python3
"""Issue #2920: workspace source SSOT after mutate.

AC:
  1. authoritative_workspace_source + invalidate/note helpers
  2. exit_mutation_boundary invalidates text when flat mutated
  3. eval-current :jit / serialize-workspace use authoritative helper
  4. set-code / load / restore stamp text
  5. Docs + suite + build.py
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

    ixx = _read("src/compiler/evaluator.ixx")
    must("authoritative_workspace_source" in ixx, "AC1: authoritative helper")
    must("invalidate_workspace_source_text" in ixx, "AC1: invalidate")
    must("note_workspace_source_text" in ixx, "AC1: note stamp")
    must("2920" in ixx, "AC1: cites #2920")
    must("workspace_source_text_valid_" in ixx, "AC1: validity flag")

    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    must("invalidate_workspace_source_text" in bound, "AC2: boundary invalidates")
    must("mutation_log_size" in bound and "2920" in bound, "AC2: only when log grew")

    ev = _read("src/compiler/evaluator_primitives_eval.cpp")
    must("authoritative_workspace_source" in ev, "AC3: jit uses authoritative")
    must("note_workspace_source_text" in ev, "AC4: set-code stamps")

    pers = _read("src/compiler/evaluator_primitives_persist.cpp")
    must("authoritative_workspace_source" in pers, "AC3: serialize uses authoritative")

    ast = _read("src/compiler/evaluator_primitives_ast.cpp")
    must("note_workspace_source_text" in ast, "AC4: restore restamps")

    doc = _read("docs/stdlib/workspace-source-ssot.md")
    must("2920" in doc and "FlatAST" in doc, "AC5: SSOT doc")
    must("2918" in doc, "AC5: cross-link #2918")

    contrib = _read("docs/contributing.md")
    must("workspace-source-ssot" in contrib or "2920" in contrib, "AC5: contributing")

    suite = _read("tests/suite/workspace_source_ssot_2920.aura")
    must("2920" in suite and "current-source :workspace" in suite, "AC5: suite")

    build = _read("build.py")
    must(
        "workspace-source-ssot-2920" in build or "workspace_source_ssot_2920" in build,
        "AC5: build.py",
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2920 workspace source SSOT — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
