#!/usr/bin/env python3
"""Issue #2555: TransactionGuard real path + migration coverage.

Contract:
  AC1 Real host API; scaffold simulation removed from production header
  AC2 Agent body + set-body use TransactionGuard / host factories
  AC3 Reject/metrics fields present; no default-ctor scaffold acquire
  AC4 recover_panic + PanicRecovered path cited
  AC5 test + cmake + build.py gate + self-test

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    tg = _read("src/core/transaction_guard.hh")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    mbg = _read("src/compiler/evaluator_mutation_boundary.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/core/test_transaction_guard_2555.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2555", "AC1", tg)
    must("struct TransactionGuardHost", "AC1", tg)
    must("try_acquire", "AC1", tg)
    must("host_owns_panic_checkpoint", "AC1", tg)
    must_not("simulate boundary acquisition", "AC1", tg)
    must("TransactionGuard() = delete", "AC1", tg)

    # AC2
    must("g_orch_agent_body_tx", "AC2", fiber)
    must("transaction_guard_host", "AC2", fiber)
    must("TransactionGuard", "AC2", fiber)
    must("transaction_guard_host(ev)", "AC2", mut)
    must("TransactionGuard tg", "AC2", mut)
    must_not("TransactionGuard surface is also exercised", "AC2", mut)
    must("transaction_guard_try_acquire", "AC2", mbg)
    must("transaction_guard_host_for_region", "AC2", mbg)
    must("transaction_guard_host", "AC2", ixx)
    must("make_transaction_guard", "AC2", ixx)

    # AC3 / AC4 API surface
    must("Rejected", "AC3", tg)
    must("rejected_total", "AC3", tg)
    must("recover_panic", "AC4", tg)
    must("PanicRecovered", "AC4", tg)
    must("panic_recovered_total", "AC4", tg)
    must("mark_failed", "AC3", tg)
    must("commit()", "AC3", tg)

    # AC5 gate
    must("ac1_no_scaffold", "AC5", test)
    must("ac3_reject", "AC5", test)
    must("ac4_panic_recover", "AC5", test)
    must("ac5_live_and_schema", "AC5", test)
    must("test_transaction_guard_2555", "AC5", cmake)
    must("check_transaction_guard_migration_2555", "AC5", build)
    must("cmd_transaction_guard_migration_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2555 TransactionGuard real path — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
