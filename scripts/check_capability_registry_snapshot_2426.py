#!/usr/bin/env python3
"""Issue #2426: CapabilityRegistry::snapshot_registry_state() (#1840 pattern).

Contract:
  AC1 RegistryStateSnapshot + snapshot_registry_state double-check acquire
  AC2 concurrent grant/revoke/snapshot gate test
  AC3 atomic sandbox_mode / default_tenant + explicit memory orders in snapshot
  AC4 existing assignment/conversion still present; gate wired

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
            fails.append(f"{label}: unexpected {n!r}")

    hh = _read("src/core/capability_model.hh")
    test = _read("tests/core/test_capability_registry_snapshot_2426.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2426", "AC1", hh)
    must("struct RegistryStateSnapshot", "AC1", hh)
    must("snapshot_registry_state()", "AC1", hh)
    must("2426 AC1", "AC1", test)

    must("2426 AC2", "AC2", test)
    must("concurrent grant", "AC2", test)

    must("struct AtomicEffectSandboxMode", "AC3", hh)
    must("struct AtomicTenantId", "AC3", hh)
    must("sandbox_mode.load(std::memory_order_acquire)", "AC3", hh)
    must("default_tenant.load(std::memory_order_acquire)", "AC3", hh)
    must("grant_min_valid_epoch_.load(std::memory_order_acquire)", "AC3", hh)
    must("hard_fiber_isolation_.load(std::memory_order_acquire)", "AC3", hh)
    # CapabilityRegistry fields are atomic wrappers (not plain enum/TenantId).
    must("AtomicEffectSandboxMode sandbox_mode{}", "AC3", hh)
    must("AtomicTenantId default_tenant{}", "AC3", hh)
    must("2426 AC3", "AC3", test)

    must("operator=(EffectSandboxMode m)", "AC4", hh)
    must("2426 AC4", "AC4", test)
    must("check_capability_registry_snapshot_2426", "gate", build)
    must("cmd_capability_registry_snapshot_coverage", "gate", build)
    must("test_capability_registry_snapshot_2426", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: capability registry snapshot #2426 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
