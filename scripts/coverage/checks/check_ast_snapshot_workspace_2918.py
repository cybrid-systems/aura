#!/usr/bin/env python3
"""Issue #2918: ast:snapshot / ast:diff use workspace source (:workspace).

AC:
  1. ast:snapshot does not call bare current-source ()
  2. Uses :workspace / workspace_source helper; fails when no workspace
  3. ast:diff same
  4. Aura stdlib safe-refactor / refactor / workspace prefer :workspace
  5. Suite + build.py wiring
"""

from __future__ import annotations

import re
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

    ast = _read("src/compiler/evaluator_primitives_ast.cpp")
    must("Issue #2918" in ast, "AC1: ast TU cites #2918")
    must("workspace_source_string" in ast or '":workspace"' in ast, "AC1: workspace helper")
    must("workspace_flat_" in ast, "AC1: checks workspace_flat_")

    # Bare current-source () must not appear in snapshot/diff paths.
    # Allow lookup("current-source") but not (*)({}) empty call for source.
    bare_call = re.compile(r"\(\*src_fn\)\s*\(\s*\{\s*\}\s*\)")
    # Restrict to lines near snapshot/diff by scanning after labels.
    for label in ('add("ast:snapshot"', 'add("ast:diff"'):
        if label not in ast:
            fails.append(f"missing {label}")
            continue
        chunk = ast.split(label, 1)[1][:2500]
        if bare_call.search(chunk):
            fails.append(f"AC1/2: bare current-source () still in {label}")
        if "workspace_source" not in chunk and ":workspace" not in chunk:
            fails.append(f"AC1/2: no workspace source in {label}")

    must(
        "no silent empty" in ast.lower() or "null → -1" in ast or "hard fail" in ast.lower(),
        "AC3: document fail when no workspace",
    )

    eval_cpp = _read("src/compiler/evaluator_primitives_eval.cpp")
    must("2918" in eval_cpp, "AC: current-source docs cite #2918")

    for path, needle in (
        ("lib/std/safe-refactor.aura", "current-source :workspace"),
        ("lib/std/refactor.aura", "current-source :workspace"),
        ("lib/std/workspace.aura", "current-source :workspace"),
    ):
        t = _read(path)
        must(needle in t or "(current-source :workspace)" in t, f"AC4: {path} uses :workspace")
        # bare (current-source) for user-script intent should be gone from these call sites
        if path.endswith("safe-refactor.aura"):
            must(
                "(current-source)" not in t.replace("(current-source :workspace)", ""),
                f"AC4: {path} residual bare current-source",
            )

    suite = _read("tests/suite/ast_snapshot_workspace_2918.aura")
    must("2918" in suite and "ast:snapshot" in suite, "AC5: suite present")
    must("current-source :workspace" in suite or "(current-source :workspace)" in suite, "AC5: suite uses :workspace")
    must("set-code" in suite, "AC5: suite set-code path")

    build = _read("build.py")
    must("ast-snapshot-workspace-2918" in build or "ast_snapshot_workspace_2918" in build, "AC5: build.py gate")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2918 ast:snapshot/diff workspace source — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
