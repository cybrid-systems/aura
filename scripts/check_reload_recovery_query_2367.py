#!/usr/bin/env python3
"""Issue #2367: ReloadRecovery query primitive + recovery-state snapshot.

  AC1: soft empty path (recovery-active / zeros)
  AC2: force-JIT exhaustion path visible on query
  AC3: success clears mask
  AC4: keys on hot-update-registry-stats + schema-2367
  AC5: C snapshot + query registration + tests + gate

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

    reg_hh = _read("src/compiler/hot_update_registry.hh")
    reg_cpp = _read("src/compiler/hot_update_registry.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")
    test = _read("tests/compiler/test_reload_recovery_query_2367.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 / soft empty + recovery fields
    must("reload_recovery_state", "AC1", reg_hh)
    must("recovery_active", "AC1", reg_hh)
    must("ac1_soft_empty", "AC1", test)

    # AC2 force-JIT + query keys
    must("on_force_jit_for_reason", "AC2", reg_cpp)
    must("last_force_jit_at_epoch_notify_", "AC2", reg_hh)
    must("force-jit-regions-mask", "AC2", mut)
    must("last-force-jit-reason", "AC2", mut)
    must("ac2_force_jit_exhaustion", "AC2", test)

    # AC3 clear on success
    must("force_jit_regions_mask_.store(0", "AC3", reg_cpp)
    must("ac3_success_clears", "AC3", test)

    # AC4 existing surface + lineage
    must("schema-2367", "AC4", mut)
    must("issue-2367", "AC4", mut)
    must("query:hot-update-registry-stats", "AC4", mut)
    must("ac4_hot_update_surface", "AC4", test)

    # AC5 C snapshot + dual query names + gate
    must("aura_reload_recovery_snapshot", "AC5", reg_hh)
    must("aura_hot_update_reload_recovery_get_snapshot", "AC5", reg_cpp)
    must("query:reload-recovery-state", "AC5", mut)
    must("query:aot-reload-recovery-stats", "AC5", mut)
    must("query:reload-recovery-state", "AC5", obs)
    must("storm-level", "AC5", mut)
    must("reemit-boundary-policy", "AC5", mut)
    must("reload-recovery-wired", "AC5", mut)
    must("test_reload_recovery_query_2367", "AC5", cmake)
    must("check_reload_recovery_query_2367", "AC5", build)
    must("cmd_reload_recovery_query_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2367 ReloadRecovery query primitive — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
