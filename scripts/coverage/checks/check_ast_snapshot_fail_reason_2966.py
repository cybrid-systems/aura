#!/usr/bin/env python3
"""Issue #2966: ast:snapshot failure is observable (never silent -1 only).

Contract:
  AC1 Fail paths publish last reason (guard / no-workspace / empty-source)
  AC2 (ast:snapshot-fail-reason) keyword + query keys; still returns -1
  AC3 set-code bootstrap → id>=0; define-only denseness path documented
  AC4 schema-2966; preserve #2918 workspace source path
  AC5 Extend test_current_source_roundtrip; source-cite; no docs/design/*
  AC6 Linter + build.py gate

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

    ast = _read("src/compiler/evaluator_primitives_ast.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    q = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    t = _read("tests/compiler/test_current_source_roundtrip.cpp")
    build = _read("build.py")

    # AC1
    must("#2966", "AC1", ast)
    must("note_ast_snapshot_fail", "AC1", ast)
    must("kSnapFailNoWorkspace", "AC1", ast)
    must("kSnapFailEmptySource", "AC1", ast)
    must("kSnapFailGuard", "AC1", ast)

    # AC2
    must("ast:snapshot-fail-reason", "AC2", ast)
    must(":no-workspace", "AC2", ast)
    must("last-ast-snapshot-fail-reason", "AC2", q)
    must("ast-snapshot-fail-total", "AC2", q)
    must("schema-2966", "AC2", q)

    # AC3 contract
    must("set-code", "AC3", ast)
    must("Top-level", "AC3", ast)
    must("ac2966_1_no_workspace_observable", "AC3", t)
    must("ac2966_2_set_code_path_ok", "AC3", t)

    # AC4 lineage #2918
    must("2918", "AC4", ast)
    must("workspace unparse", "AC4", ast)
    must("last_ast_snapshot_fail_reason", "AC4", ixx)

    # AC5
    must("ac2966_4_source_cite", "AC5", t)
    must("check_ast_snapshot_fail_reason_2966", "AC5", build)

    # AC6
    must("cmd_ast_snapshot_fail_reason_2966_coverage", "AC6", build)
    must("ast-snapshot-fail-reason-2966", "AC6", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2966-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2966.cpp").is_file():
        fails.append("tests/compiler/test_issue_2966.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2966 ast:snapshot fail reason — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
