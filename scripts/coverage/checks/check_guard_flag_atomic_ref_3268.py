#!/usr/bin/env python3
"""Issue #3268: MutationBoundaryGuard flag_ atomic_ref fail-close + region_lock_ move.

Caller-owned success flag stays bool* (try_acquire / C ABI / stack locals)
but must be fiber-local. Guard fail-close is a single atomic_ref exchange
so cancel-poll cannot tear true→act. region_lock_ is unique_lock and
moved (moved-from dtor is a no-op). TLS rebind after source reset
depends on the move ctor remaining noexcept.

Contract:
  AC1  cancel-poll uses success_flag_exchange_false; bool* API stays
  AC2  region_lock_ is unique_lock moved, not copied
  AC3  g_tls_outermost_guard = this after source reset; noexcept comment
  AC4  stack bool try_acquire + mark_failed still works (zero extra)
  AC5  extend test_mutation_guard_unit_batch; linter after #3267; no invent

Exit 0 = all rows satisfied.

Follow-up #3269: arena compact/defrag TOCTOU — unique workspace after TLS skip.
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

    cpp = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    fib = _read("src/compiler/evaluator_fiber_mutation.cpp")
    tree = _read("src/compiler/evaluator_workspace_tree.cpp")
    test = _read("tests/compiler/test_mutation_guard_unit_batch.cpp")
    build = _read("build.py")
    l3267 = _read("scripts/coverage/checks/check_env_publish_lock_3267.py")

    pos = cpp.find("Issue #3268: single exchange")
    win = cpp[pos : pos + 500] if pos >= 0 else ""
    must("success_flag_exchange_false(flag_)", "AC1 cancel exchange", win)
    must("std::atomic_ref<bool>(*f).exchange(false", "AC1 helper", ixx)
    must("fiber-local", "AC1 contract", ixx)
    must("bool* flag_", "AC1 bool* stays", ixx)
    must("bool* success_flag = nullptr", "AC1 try_acquire API", ixx)
    if "if (flag_ && *flag_)" in cpp:
        fails.append("AC1: load-then-store RMW still present")
    must("ac3268_1_cancel_exchange", "AC1 test", test)

    mpos = cpp.find("MutationBoundaryGuard::MutationBoundaryGuard(MutationBoundaryGuard&& o) noexcept")
    mwin = cpp[mpos : mpos + 5000] if mpos >= 0 else ""
    must("region_lock_(std::move(o.region_lock_))", "AC2 move", mwin)
    if "region_lock_(o.region_lock_)" in mwin:
        fails.append("AC2: region_lock_ copied, not moved")
    must("std::unique_lock<std::mutex> region_lock_", "AC2 type", ixx)
    must("Moved-from owns nothing", "AC2 moved-from comment", ixx)
    must("ac3268_2_region_lock_move", "AC2 test", test)

    must("g_tls_outermost_guard = this", "AC3 TLS", mwin)
    must("this ctor is noexcept", "AC3 noexcept comment", mwin)
    reset = mwin.find("o.ev_ = nullptr")
    tls = mwin.find("g_tls_outermost_guard = this")
    if reset < 0 or tls < 0 or reset > tls:
        fails.append("AC3: TLS assign must stay after source reset")
    must("ac3268_3_tls_noexcept", "AC3 test", test)

    must("success_flag_exchange_false(flag_)", "AC4 mark_failed path", ixx)
    must("success_flag_exchange_false(outermost_mutation_success_flag_)", "AC4 fiber", fib)
    must("success_flag_load(outermost_mutation_success_flag_)", "AC4 workspace", tree)
    must("ac3268_4_stack_bool_api", "AC4 test", test)

    must("ac3268_5_source_and_linter", "AC5 test", test)
    must("check_guard_flag_atomic_ref_3268", "AC5 build.py", build)
    prev = build.find("check_env_publish_lock_3267")
    ours = build.find("check_guard_flag_atomic_ref_3268")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3267")
    must("3268", "AC5 extend 3267 linter", l3267)
    if (ROOT / "tests" / "issues" / "test_issue_3268.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3268.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3268.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3268.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3268-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3268" in q or "schema-3268" in test:
        fails.append("AC5: new schema-3268 query key (SlimSurface)")

    if fails:
        print("FAIL #3268 guard_flag_atomic_ref:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3268 guard_flag_atomic_ref: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
