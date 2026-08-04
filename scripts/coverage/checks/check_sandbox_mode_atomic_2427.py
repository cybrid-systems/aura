#!/usr/bin/env python3
"""Issue #2427: CapabilityRegistry::sandbox_mode (and default_tenant) atomic.

Contract:
  AC1 AtomicEffectSandboxMode with std::atomic uint8 + is_always_lock_free
  AC2 concurrent setter + reader gate test
  AC3 assignment/load release-acquire preserved for callers
  AC4 EffectAuditEntry.sandbox_mode stamped in record_audit

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    hh = _read("src/core/capability_model.hh")
    test = _read("tests/core/test_sandbox_mode_atomic.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2427", "AC1", hh)
    must("struct AtomicEffectSandboxMode", "AC1", hh)
    must("std::atomic<std::uint8_t> v", "AC1", hh)
    must("AtomicEffectSandboxMode sandbox_mode{}", "AC1", hh)
    must("is_always_lock_free", "AC1", hh)
    must("2427 AC1", "AC1", test)

    must("2427 AC2", "AC2", test)
    must("concurrent sandbox_mode", "AC2", test)

    must("std::memory_order_release", "AC3", hh)
    must("std::memory_order_acquire", "AC3", hh)
    must("operator=(EffectSandboxMode m)", "AC3", hh)
    must("2427 AC3", "AC3", test)

    must("sandbox_mode = EffectSandboxMode::Off", "AC4", hh)  # field default on entry
    must("entry.sandbox_mode = sandbox_mode.load", "AC4", hh)
    must("2427 AC4", "AC4", test)

    must("check_sandbox_mode_atomic_2427", "gate", build)
    must("cmd_sandbox_mode_atomic_coverage", "gate", build)
    must("test_sandbox_mode_atomic", "gate", cmake)

    # F4 together
    must("struct AtomicTenantId", "F4", hh)
    must("AtomicTenantId default_tenant{}", "F4", hh)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: sandbox_mode atomic #2427 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
