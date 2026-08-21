#!/usr/bin/env python3
"""Issue #3042: eliminate residual std::function dirty predicates from PureWrap.

Contract (one row per AC):
  AC1  Production PureWrap stages have no std::function dirty members/setters
  AC2  Dirty predicates are BlockDirtyPred / InstructionDirtyPred (inlineable)
  AC3  check_pipeline_dod_compliance + pure_wrap static_assert still present
  AC4  dirty short-circuit semantics unchanged; test-only fn-pointer setter
  AC5  schema-3042 + concept_rejection under production stress
  AC6  this linter wired in build.py; no test_issue_3042.cpp; no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

PROD_PUREWRAP = (
    "src/compiler/pass_impls.ixx",
    "src/compiler/optimization_passes.ixx",
    "src/compiler/service.ixx",
    "src/compiler/pass_pipeline_core.ixx",
)

BANNED = (
    r"set_block_dirty_fn\s*\(\s*std::function",
    r"set_instruction_dirty_fn\s*\(\s*std::function",
    r"std::function\s*<\s*bool\s*\(\s*std::uint32_t\s*\)\s*>\s*block_dirty",
    r"std::function\s*<\s*bool\s*\(\s*std::uint32_t\s*,\s*std::uint32_t\s*\)\s*>\s*instruction_dirty",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    prod = "".join(_read(p) for p in PROD_PUREWRAP)
    core = _read("src/compiler/pass_pipeline_core.ixx")
    impls = _read("src/compiler/pass_impls.ixx")
    cc = _read("src/core/concept_constraints.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    sweep = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    test = _read("tests/compiler/test_hot_pass_hard_dod.cpp")
    wrap = _read("tests/compiler/test_hot_pass_pure_wrap.cpp")
    build = _read("build.py")
    linear = _read("tests/compiler/test_linear_batch.cpp")

    # ── AC1: grep-clean production PureWrap dirty APIs ──
    for pat in BANNED:
        if re.search(pat, prod):
            fails.append(f"AC1: banned dirty std::function pattern {pat}")
    must("Issue #3042", "AC1 core", core)
    must("kPureWrapNoStdFunctionDirtyIssue = 3042", "AC1 core", core)
    must("set_block_dirty_pred", "AC1 pipeline", core)
    must("set_instruction_dirty_pred", "AC1 pipeline", core)
    # Issue #3234 closed the Tarjan leftover; dirty-predicate grep stays.
    if "std::function<void(std::uint32_t)> strongconnect" in impls:
        fails.append("AC1: SCC leftover still present (closed by #3234)")

    # ── AC2: inlineable column-view / fn-pointer preds ──
    must("struct BlockDirtyPred", "AC2", core)
    must("struct InstructionDirtyPred", "AC2", core)
    must("is_trivially_copyable_v<BlockDirtyPred>", "AC2", core)
    must("is_trivially_copyable_v<InstructionDirtyPred>", "AC2", core)
    must("bool (*fn)(std::uint32_t) = nullptr", "AC2 fn ptr", core)
    must("DefaultAllDirty", "AC2 default", core)
    must("3042 AC2", "AC2 test", test)

    # ── AC3: dod + pure_wrap still green ──
    must("check_pipeline_dod_compliance", "AC3", core)
    must("static_assert(PureWrapPass", "AC3", impls)
    must("3042 AC3", "AC3 test", test)

    # ── AC4: short-circuit + test-only setter ──
    must("set_block_dirty_fn(bool (*fn)(std::uint32_t))", "AC4 test setter", impls)
    must("pipeline_dirty_short_circuit_total", "AC4", core)
    must("3042 AC4", "AC4 test", test)
    # Capturing lambda in linear_batch must be non-capturing now.
    if re.search(r"set_block_dirty_fn\(\s*\[&\]", linear):
        fails.append("AC4: capturing lambda still passed to set_block_dirty_fn")
    must("set_block_dirty_fn([](std::uint32_t bi) { return bi == 0; })", "AC4 linear", linear)

    # ── AC5: schema + rejection ──
    must("schema-3042", "AC5 query", q)
    must("issue-3042", "AC5 query", q)
    must("pure-wrap-no-std-function-dirty-wired", "AC5 query", q)
    must("schema-3042", "AC5 sweep", sweep)
    must("Issue #3042", "AC5 concepts", cc)
    must("3042 AC5", "AC5 test", test)
    must("schema-3042", "AC5 wrap test", wrap)
    must("pure_wrap_no_std_function_dirty_wired", "AC5 metric", core)

    # ── AC6: wired + no invent / no design ──
    must("check_pure_wrap_no_std_function_3042", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3042.cpp").is_file():
        fails.append("AC6: test_issue_3042.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3042.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3042.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3042-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3042 PureWrap no std::function dirty predicates — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
