#!/usr/bin/env python3
"""Issue #3352: TransformEngine / AutoFixEngine Guard wrap.

Residual after #3074 sole Guard: C++ Transform/AutoFix still wrote
FlatAST via apply_patches with no MutationBoundaryGuard. Optional
Evaluator* on query_and_fix / run_all: when set, wrap with
mutate_dispatch_try_acquire; when absent (CLI throwaway), keep today's
path and do not hand the mutated tree to a live Evaluator without re-load.

Contract:
  AC1 in-process ev → mutate_dispatch_try_acquire
  AC2 CLI throwaway no Evaluator; no live handoff without re-load
  AC3 Soft / no ev → 0 extra; no schema-3352 / g_3352_*
  AC4 after #3074; no invent / docs/design / new mutate:* key

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

    qixx = _read("src/compiler/query.ixx")
    impl = _read("src/compiler/query_impl.cpp")
    main_cpp = _read("src/main.cpp")
    test = _read("tests/compiler/test_mutation_guard_try_acquire_unit.cpp")
    build = _read("build.py")

    must("kTransformEngineGuardIssue = 3352", "AC1 stamp", qixx)
    must("Evaluator* ev = nullptr", "AC1 optional ev", qixx)
    must("mutate_dispatch_try_acquire", "AC1 wrap", impl)
    must("if (ev)", "AC1 ev gate", impl)
    must("ac3352_1_in_process_guard", "AC1 test", test)

    must("query_and_fix(engine, argv[2], argv[3])", "AC2 CLI query-and-fix", main_cpp)
    must("fixer.run_all()", "AC2 CLI auto-fix", main_cpp)
    if "query_and_fix(engine, argv[2], argv[3], " in main_cpp:
        fails.append("AC2: CLI --query-and-fix must not pass Evaluator")
    must("Do not hand this mutated tree to a live", "AC2 no-handoff cite", main_cpp)
    must("Do not pass this mutated FlatAST into a live Evaluator", "AC2 auto-fix no-handoff", main_cpp)
    must("ac3352_2_cli_offline", "AC2 test", test)

    must("if (ev)", "AC3 ev-null skip", impl)
    must("ac3352_3_soft_no_ev_zero_extra", "AC3 test", test)
    if "schema-3352" in impl or "schema-3352" in qixx:
        fails.append("AC3: new schema-3352 query key")
    if "g_3352_" in impl or "g_3352_" in qixx:
        fails.append("AC3: new g_3352_* counter")
    if 'add_mutate("mutate:' in impl:
        fails.append("AC3: new public mutate:* key")

    must("check_transform_engine_guard_3352", "AC4 build.py", build)
    must("check_mutate_dispatch_sole_guard_3074", "AC4 after #3074", build)
    i3074 = build.find("check_mutate_dispatch_sole_guard_3074.py")
    i3352 = build.find("check_transform_engine_guard_3352.py")
    if i3074 < 0 or i3352 < 0 or i3352 < i3074:
        fails.append("AC4: #3352 linter must run after #3074")
    must("ac3352_4_run_all_once_and_linter", "AC4 test", test)
    must("/*ev=*/nullptr", "AC4 run_all inner nullptr", impl)
    if (ROOT / "tests" / "issues" / "test_issue_3352.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3352.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3352.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3352.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3352-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3352 transform_engine_guard:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3352 transform_engine_guard: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
