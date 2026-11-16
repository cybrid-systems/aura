#!/usr/bin/env python3
"""Issue #2918: ast:snapshot / ast:diff use workspace source (:workspace).

AC:
  1. ast:snapshot does not call bare current-source ()
  2. Uses :workspace / workspace_source helper; fails when no workspace
  3. ast:diff same
  4. Aura stdlib safe-refactor / refactor / workspace prefer :workspace
  5. Suite + gate wiring
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
    # #5142/#5143: the registration may be line-wrapped and may now be
    # add_hidden(), so anchor on the name literal, not the call shape.
    for label in ('"mw:snapshot"', '"qr:diff"'):
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

    # Issue #5252: refactor / safe-refactor no longer read the workspace
    # source at all — they drive the registered mutate:* small steps
    # (rename-symbol / inline-call / check+rebind). A user-script-intent
    # source read cannot be mis-targeted to the eval frame if there is no
    # read left. The #2918 contract for these two files is therefore: any
    # (current-source) they still contain must name :workspace (a bare
    # read goes back to the eval frame, which is the bug #2918 fixed).
    for path in ("lib/std/safe-refactor.aura", "lib/std/refactor.aura"):
        t = _read(path)
        must(
            "(current-source)" not in t.replace("(current-source :workspace)", ""),
            f"AC4: {path} has no bare current-source (Issue #5252)",
        )

    # #4274: std/workspace no longer reads eval-frame source. Merge goes
    # through workspace:sync-from; snapshots go through ast:snapshot, which
    # is the C++ workspace-source path. Bare current-source must stay gone.
    ws = _read("lib/std/workspace.aura")
    must("(mutate :sync-from" in ws, "AC4: lib/std/workspace.aura merges via sync-from")
    # Issue #5009: the write surface is (mutate :op); ast:snapshot is the
    # deprecated alias it forwards to.
    must(
        "(mutate :snapshot" in ws or "mw:snapshot" in ws,
        "AC4: lib/std/workspace.aura snapshots via (mutate :snapshot)",
    )
    must("(current-source)" not in ws, "AC4: lib/std/workspace.aura residual bare current-source")

    suite = _read("tests/suite/ast_snapshot_workspace_2918.aura")
    must("2918" in suite and "(mutate :snapshot" in suite, "AC5: suite present")
    must("current-source :workspace" in suite or "(current-source :workspace)" in suite, "AC5: suite uses :workspace")
    must("set-code" in suite, "AC5: suite set-code path")

    # Issue #5246: wired through the shared coverage runner — run_checks.py
    # globs scripts/coverage/checks/check_*.py, replacing the deleted build.py
    # `or cmd_*_coverage()` wrapper.
    must(
        (ROOT / "scripts" / "coverage" / "checks" / "check_ast_snapshot_workspace_2918.py").is_file()
        and "discover_checks" in _read("scripts/coverage/run_checks.py"),
        "AC5: coverage runner",
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2918 ast:snapshot/diff workspace source — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
