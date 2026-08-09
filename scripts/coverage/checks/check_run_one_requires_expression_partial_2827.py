#!/usr/bin/env python3
"""Issue #2827: run_one epoch sync gates on set_pipeline_epoch alone.

Contract (one row per AC):
  AC1 run_one cites #2827; kHasSetPipelineEpoch; partial_skipped metric
  AC2 set gated alone (not AND with pipeline_epoch_hint in one requires)
  AC3 test suite present (SetOnly / HintOnly / Dual)
  AC4 linter wired; schema-2827; no docs/design/2827-*; no test_issue_2827.cpp

Exit 0 = all rows satisfied.
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

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    core = _read("src/compiler/pass_pipeline_core.ixx")
    obs = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    test = _read("tests/compiler/test_run_one_requires_expression_partial.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = core.find("execute a single pass")
    if pos < 0:
        pos = core.rfind("bool run_one")
    body = core[pos : pos + 4500] if pos >= 0 else ""

    # AC1
    must("Issue #2827", "AC1", body)
    must("kHasSetPipelineEpoch", "AC1", body)
    must("kHasPipelineEpochHint", "AC1", body)
    must("pipeline_epoch_sync_partial_skipped_total", "AC1", body)
    must("pipeline_epoch_sync_partial_skipped_total", "AC1", core)

    # AC2: set alone; no dual-requires AND as the only gate
    must("if constexpr (kHasSetPipelineEpoch)", "AC2", body)
    must("set_pipeline_epoch(epoch)", "AC2", body)
    # Reject a single requires block that still AND-binds both methods.
    dual_and = re.search(
        r"requires\s*\(\s*P\s*&\s*p\s*\)\s*\{[^}]*set_pipeline_epoch[^}]*"
        r"pipeline_epoch_hint[^}]*\}",
        body,
        re.DOTALL,
    )
    if dual_and:
        fails.append("AC2: residual dual AND requires(set + hint) in run_one body")

    # AC3
    must("ac2827", "AC3", test)
    must("2827", "AC3", test)
    must("SetOnlyEpochPass", "AC3", test)
    must("HintOnlyEpochPass", "AC3", test)
    must("DualEpochPass", "AC3", test)
    must("pipeline_epoch_sync_partial_skipped_total", "AC3", test)
    must("run_one", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_run_one_requires_expression_partial.cpp").is_file():
        fails.append("AC3: missing test_run_one_requires_expression_partial.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2827.cpp").is_file():
        fails.append("AC3: test_issue_2827.cpp present (forbidden per #81967)")
    must("test_run_one_requires_expression_partial", "AC3", cmake)

    # AC4
    must("check_run_one_requires_expression_partial_2827", "AC4", build)
    must("schema-2827", "AC4", obs)
    must("pipeline-epoch-sync-partial-skipped-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2827-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2827 run_one requires-expression partial — set-only gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
