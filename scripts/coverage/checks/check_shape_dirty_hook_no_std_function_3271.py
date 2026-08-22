#!/usr/bin/env python3
"""Issue #3271: ShapeProfiler dirty hook is a fn ptr (no std::function).

set_dirty_hook stored std::function; under multi-fiber AI mutate+eval the
hook is the shape-invalidate → IR/dirty cascade. Type erasure blocked
inlining and could allocate. Production path is DirtyHookFn + atomic
store/load after shard unlock. Unset is a null load (zero extra).

Contract:
  AC1  no std::function< in shape_profiler.h/.cpp; DirtyHookFn + atomic
  AC2  fire after shard unlock; service trampoline (no capturing lambda)
  AC3  unset = nullptr; no dirty_hook_copy
  AC4  hook path does not take meta_mtx_
  AC5  extend test_shape_profiler_concurrency; linter after #3270; no invent
  AC6  this linter mirrors #3042 PureWrap no-std::function discipline

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

    hh = _read("src/compiler/shape_profiler.h")
    cpp = _read("src/compiler/shape_profiler.cpp")
    svc = _read("src/compiler/service.ixx")
    test = _read("tests/compiler/test_shape_profiler_concurrency.cpp")
    build = _read("build.py")
    l3270 = _read("scripts/coverage/checks/check_primid_drift_3270.py")

    must("kShapeDirtyHookNoStdFunctionIssue = 3271", "AC1 stamp", hh)
    must("using DirtyHookFn = void (*)(FnKey, std::uint32_t) noexcept", "AC1 alias", hh)
    must("std::atomic<DirtyHookFn> dirty_hook_", "AC1 atomic", hh)
    must("is_trivially_copyable_v<DirtyHookFn>", "AC1 trivially copyable", hh)
    must("void set_dirty_hook(DirtyHookFn hook) noexcept", "AC1 setter", hh)
    if "std::function<" in hh:
        fails.append("AC1: shape_profiler.h still has std::function<")
    if "std::function<" in cpp:
        fails.append("AC1: shape_profiler.cpp still has std::function<")

    must("Issue #3271: fn-ptr load after shard unlock", "AC2 record/invalidate", cpp)
    must("Issue #3271: load after all shard unique locks drop", "AC2 compact", cpp)
    must("shape_dirty_hook_trampoline", "AC2 trampoline", svc)
    must("set_dirty_hook(&CompilerService::shape_dirty_hook_trampoline)", "AC2 wire", svc)
    if "set_dirty_hook([this]" in svc:
        fails.append("AC2: capturing lambda still wires set_dirty_hook")

    must("Soft unset is a null load", "AC3 quiet", cpp)
    if "dirty_hook_copy" in cpp:
        fails.append("AC3: residual dirty_hook_copy (std::function snapshot)")
    must("dirty_hook_.store(hook, std::memory_order_release)", "AC3 set", cpp)

    must("no meta_mtx_ on the hook path", "AC4 no meta", cpp)
    must("ac3271_4_hook_path_no_meta", "AC4 test", test)

    must("ac3271_1_no_std_function", "AC5 test AC1", test)
    must("ac3271_2_fire_after_unlock", "AC5 test AC2", test)
    must("ac3271_3_unset_zero_extra", "AC5 test AC3", test)
    must("ac3271_5_source_and_linter", "AC5 test AC5", test)
    must("check_shape_dirty_hook_no_std_function_3271", "AC5 build.py", build)
    prev = build.find("check_primid_drift_3270")
    ours = build.find("check_shape_dirty_hook_no_std_function_3271")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3270")
    must("3271", "AC5 extend 3270 linter", l3270)
    if (ROOT / "tests" / "issues" / "test_issue_3271.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3271.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3271.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3271.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3271-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3271" in q or "schema-3271" in test:
        fails.append("AC5: new schema-3271 query key (SlimSurface)")

    must(
        "BlockDirtyPred must be inlineable (no std::function) (#3042)",
        "AC6 #3042 lineage",
        _read("src/compiler/pass_pipeline_core.ixx"),
    )

    if fails:
        print("FAIL #3271 shape_dirty_hook:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3271 shape_dirty_hook: fn ptr, post-unlock fire, no std::function")
    return 0


if __name__ == "__main__":
    sys.exit(main())
