#!/usr/bin/env python3
"""Issue #2830: DCEPass expands Call/Apply/PrimCall arg ranges.

Contract (one row per AC):
  AC1 mark_used_slots; Call/Apply/PrimCall expanded ranges; Issue #2830
  AC2 operand_under_count metric
  AC3 test suite present (5-arg Call)
  AC4 linter wired; schema-2830; no docs/design/2830-*; no test_issue_2830.cpp

Exit 0 = all rows satisfied.
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

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    impls = _read("src/compiler/pass_impls.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_dce_pass_variable_args.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 — definition window (not the comment near operand_count_local).
    must("Issue #2830", "AC1", impls)
    must("mark_used_slots", "AC1", impls)
    pos = impls.find("static void mark_used_slots")
    if pos < 0:
        pos = impls.find("void mark_used_slots")
    body = impls[pos : pos + 1800] if pos >= 0 else ""
    must("case O::Call:", "AC1", body)
    must("case O::Apply:", "AC1", body)
    must("case O::PrimCall:", "AC1", body)
    must("base + i", "AC1", body)
    # DCEPass::run_on_block (local_count param) must call mark_used_slots.
    run = impls.find("void run_on_block(aura::ir::BasicBlock& block, std::uint32_t local_count")
    if run < 0:
        run = impls.find("void run_on_block(aura::ir::BasicBlock& block, std::uint32_t local_count = 0)")
    run_body = impls[run : run + 1600] if run >= 0 else ""
    must("mark_used_slots", "AC1", run_body)

    # AC2
    must("operand_under_count_total", "AC2", impls)
    must("operand_under_count_total_", "AC2", impls)

    # AC3
    must("ac2830", "AC3", test)
    must("2830", "AC3", test)
    must("IROpcode::Call", "AC3", test)
    must("arg_count=5", "AC3", test)
    must("DCEPass", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_dce_pass_variable_args.cpp").is_file():
        fails.append("AC3: missing test_dce_pass_variable_args.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2830.cpp").is_file():
        fails.append("AC3: test_issue_2830.cpp present (forbidden per #81967)")
    must("test_dce_pass_variable_args", "AC3", cmake)

    # AC4
    must("check_dce_pass_variable_args_2830", "AC4", build)
    must("schema-2830", "AC4", obs)
    must("dce-operand-under-count-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2830-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2830 DCEPass variable-arg Call/Apply/PrimCall scan")
    return 0


if __name__ == "__main__":
    sys.exit(main())
