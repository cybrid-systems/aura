#!/usr/bin/env python3
"""Issue #2823: run_one performs fiber yield action when policy hook is true.

Contract (one row per AC):
  AC1 run_one cites #2823; fiber yield action; fiber_yield_calls_total
  AC2 service policy/action split; Fiber::yield(PassPipeline) in action
  AC3 test suite present
  AC4 linter wired; schema-2823; no docs/design/2823-*; no test_issue_2823.cpp

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

    core = _read("src/compiler/pass_pipeline_core.ixx")
    svc = _read("src/compiler/service.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_run_one_yield_hook_actual.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = core.find("execute a single pass")
    if pos < 0:
        pos = core.rfind("bool run_one")
    body = core[pos : pos + 2000] if pos >= 0 else ""

    # AC1
    must("Issue #2823", "AC1", body)
    must("g_pipeline_fiber_yield_action", "AC1", body)
    must("pipeline_fiber_yield_calls_total", "AC1", body)
    must("pipeline_yield_count.fetch_add", "AC1", body)
    must("set_pipeline_fiber_yield_action", "AC1", core)
    must("PipelineFiberYieldAction", "AC1", core)

    # AC2
    must("Issue #2823", "AC2", svc)
    must("pipeline_fiber_yield_action", "AC2", svc)
    must("YieldReason::PassPipeline", "AC2", svc)
    # Policy trampoline definition (not ctor registration call site).
    tramp = svc.find("static bool pipeline_yield_trampoline")
    if tramp < 0:
        tramp = svc.find("bool pipeline_yield_trampoline() noexcept")
    # Body only (comments above may mention the action's Fiber::yield).
    brace = svc.find("{", tramp) if tramp >= 0 else -1
    next_meth = svc.find("static void pipeline_fiber_yield_action", tramp if tramp >= 0 else 0)
    if next_meth < 0:
        next_meth = (tramp if tramp >= 0 else 0) + 300
    tramp_body = svc[brace:next_meth] if brace >= 0 else ""
    must("g_current_fiber", "AC2", tramp_body)
    if "Fiber::yield" in tramp_body:
        fails.append("AC2: pipeline_yield_trampoline body still calls Fiber::yield (policy-only)")
    act = svc.find("static void pipeline_fiber_yield_action")
    if act < 0:
        act = svc.find("void pipeline_fiber_yield_action() noexcept")
    act_body = svc[act : act + 400] if act >= 0 else ""
    must("Fiber::yield", "AC2", act_body)

    # AC3
    must("ac2823", "AC3", test)
    must("2823", "AC3", test)
    must("set_pipeline_fiber_yield_action", "AC3", test)
    must("pipeline_fiber_yield_calls_total", "AC3", test)
    must("run_one", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_run_one_yield_hook_actual.cpp").is_file():
        fails.append("AC3: missing test_run_one_yield_hook_actual.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2823.cpp").is_file():
        fails.append("AC3: test_issue_2823.cpp present (forbidden per #81967)")
    must("test_run_one_yield_hook_actual", "AC3", cmake)

    # AC4
    must("check_run_one_yield_hook_actual_2823", "AC4", build)
    must("schema-2823", "AC4", obs)
    must("pipeline-fiber-yield-calls-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2823-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2823 run_one yield hook actual — policy + Fiber::yield action")
    return 0


if __name__ == "__main__":
    sys.exit(main())
