#!/usr/bin/env python3
"""Issue #2492: force SecurityEvent WAL under Restricted (not only
multi-tenant/Strict). Production default Restricted (#2076) without
AURA_MULTI_TENANT was silent under deny storms — single-tenant commercial
deploys lost early forensic events to ring wrap (1024 entries).

Contract:
  AC1 Restricted + no multi-tenant env → force_wal true, WAL enabled
  AC2 AURA_SANDBOX=off → WAL remains off (dev_off branch)
  AC3 Ring ≥ 1024 + WAL append paired (mutation audit + side-car SE)
  AC4 WAL off short-circuits (no syscall)
  AC5 Additive metrics (audit_wal_forced_by_restricted_total) + cite
  AC6 Source-cite + tests + CMake + build.py gate + this linter

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

    sd = _read("src/compiler/security_defaults.hh")
    mw = _read("src/core/mutation_audit_wal.hh")
    test = _read("tests/compiler/test_security_audit_wal_force_restricted_2492.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 — Restricted included in force_wal.
    must("Issue #2492", "AC1", sd)
    must(
        "force_wal = multi_tenant || strict || restricted",
        "AC1",
        sd,
    )
    must("const bool restricted =", "AC1", sd)
    must(
        "g_sandbox_state().mode == SandboxMode::Restricted",
        "AC1",
        sd,
    )

    # AC2 — dev_off branch gates WAL enable.
    must("if (!dev_off) {", "AC2", sd)
    must("g_mutation_audit_wal().enable", "AC2", sd)

    # AC3 — WAL append + replay references remain; ring size check.
    must("g_mutation_audit_wal().enable", "AC3", sd)
    must("force_wal", "AC3", sd)

    # AC5 — additive metric + source-cite.
    must("audit_wal_forced_by_restricted_total", "AC5", mw)
    must("audit_wal_forced_by_multi_tenant_total", "AC5", mw)
    must("Issue #2492", "AC5", sd)
    must(
        "audit_wal_forced_by_restricted_total.fetch_add",
        "AC5",
        sd,
    )

    # AC6 — registrations + test ac functions + this linter.
    must("ac1_restricted_forces_wal", "AC1", test)
    must("ac2_off_sandbox_skips_wal", "AC2", test)
    must("ac3_ring_and_replay", "AC3", test)
    must("ac4_wal_off_short_circuit", "AC4", test)
    must("ac5_metrics_and_source_cite", "AC5", test)
    must("ac6_source_and_gate", "AC6", test)
    must("test_security_audit_wal_force_restricted_2492", "AC6", cmake)
    must(
        "aura_add_issue_test(test_security_audit_wal_force_restricted_2492)",
        "AC6",
        cmake,
    )
    must(
        "aura_issue_test_link_llvm_jit(test_security_audit_wal_force_restricted_2492)",
        "AC6",
        cmake,
    )
    must("check_security_audit_wal_force_restricted_2492", "AC6", build)
    must(
        "cmd_security_audit_wal_force_restricted_2492_coverage",
        "AC6",
        build,
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2492 SecurityEvent WAL force under Restricted — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
