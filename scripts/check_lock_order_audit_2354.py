#!/usr/bin/env python3
"""Issue #2354: debug lock-order audit coverage (scheduler / workspace /
closures / module / wait_map / fiber_registry).

  AC1: Audit off → single early branch (zero cost)
  AC2: Soft audit + correct order path
  AC3: Reverse order → hard fail / detected
  AC4: Rank table + instrumented sites source-cite
  AC5: Self-test + gate registration

Exit 0 = all ACs satisfied.
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

    h = _read("src/compiler/lock_order_audit.h")
    sch = _read("src/serve/scheduler.cpp")
    wk = _read("src/serve/worker.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    gc = _read("src/compiler/evaluator_gc.cpp")
    ml = _read("src/compiler/evaluator_module_loader.cpp")
    test = _read("tests/compiler/test_lock_order_audit_2354.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 zero cost
    must("AURA_LOCK_ORDER_AUDIT", "AC1", h)
    must("lock_order_audit_enabled", "AC1", h)
    must("if (!lock_order_audit_enabled())", "AC1", h)
    must("ac1_audit_off_zero_cost", "AC1", test)
    must("Issue #2354", "AC1", h)

    # AC2 correct order ranks
    must("Orphan = 7", "AC2", h)
    must("WaitMap = 8", "AC2", h)
    must("Joiner = 9", "AC2", h)
    must("OwnedFibers = 10", "AC2", h)
    must("FiberRegistry = 11", "AC2", h)
    must("Closures = 12", "AC2", h)
    must("Module = 13", "AC2", h)
    must("kCount = 14", "AC2", h)
    must("ac2_correct_order", "AC2", test)

    # AC3 reverse / canary
    must("dump_held_ranks", "AC3", h)
    must("std::abort()", "AC3", h)
    must("ac3_reverse_order_detected", "AC3", test)
    must("AuditedMutexLock", "AC3", h)

    # AC4 instrumented sites
    must("Level::Orphan", "AC4", sch)
    must("Level::WaitMap", "AC4", sch)
    must("Level::Joiner", "AC4", sch)
    must("Level::OwnedFibers", "AC4", sch)
    must("AuditedMutexLock", "AC4", sch)
    must("Level::FiberRegistry", "AC4", wk)
    must("Level::Workspace", "AC4", emb)
    must("Level::Closures", "AC4", gc)
    must("Level::Module", "AC4", ml)
    must("ac4_rank_table_and_source_cite", "AC4", test)

    # AC5 gate
    must("test_lock_order_audit_2354", "AC5", cmake)
    must("check_lock_order_audit_2354", "AC5", build)
    must("cmd_lock_order_audit_2354_coverage", "AC5", build)
    must("ac5_env_and_mode", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2354 lock-order audit — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
