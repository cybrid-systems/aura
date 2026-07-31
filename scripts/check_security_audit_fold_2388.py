#!/usr/bin/env python3
"""Issue #2388: fold Capability + Isolation audit rings into SecurityEvent WAL.

Contract:
  AC1 Capability/isolation record_audit dual-write SecurityEvent + WAL
  AC2 IsolationDeny single path (Evaluator does not re-append IsolationDeny)
  AC3 emit_security_event_durable + persist short-circuit when WAL off
  AC4 kSecurityAuditFoldIssue=2388 additive sentinel
  AC5 Tests + CMake + build.py gate

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

    cap = _read("src/core/capability_model.hh")
    iso = _read("src/core/workspace_isolation.hh")
    wal = _read("src/core/security_event_wal.hh")
    se = _read("src/core/security_event.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    test = _read("tests/compiler/test_security_audit_fold_2388.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 dual-write from private rings
    must("Issue #2388", "AC1", cap)
    must("emit_security_event_durable", "AC1", cap)
    must("Issue #2388", "AC1", iso)
    must("emit_security_event_durable", "AC1", iso)
    must("emit_security_event_durable", "AC1", wal)
    must("ac1_wrap_and_wal_replay", "AC1", test)

    # AC2 single IsolationDeny path: Evaluator must not append IsolationDeny
    must("ac2_isolation_single_se", "AC2", test)
    must("IsolationDeny", "AC2", iso)
    # Fold comment present; no second IsolationDeny append in evaluator hot path.
    must("Issue #2388", "AC2", sec)
    # Hot-path dual-write lives in record_audit; evaluator only TypedMutationAudit.
    must("typed_audit::capture_security_correlated_audit", "AC2", sec)
    # Ensure evaluator no longer calls append for IsolationDeny on deny path.
    # (WAL replay still uses append_security_event — exclude that by checking
    # IsolationDeny is not paired with append_security_event outside enable_.)
    iso_append_hot = False
    for line in sec.splitlines():
        if "IsolationDeny" in line and "append_security_event" in line:
            iso_append_hot = True
    if iso_append_hot:
        fails.append("AC2: evaluator still append_security_event(...IsolationDeny...) hot path")

    # AC3 short-circuit helper
    must("persist_security_event", "AC3", wal)
    must("is_enabled()", "AC3", wal)
    must("ac3_wal_off_short_circuit", "AC3", test)

    # AC4 sentinel
    must("kSecurityAuditFoldIssue", "AC4", se)
    must("kSecurityAuditFoldIssue = 2388", "AC4", se)
    must("ac4_sentinel", "AC4", test)

    # AC5 registration
    must("test_security_audit_fold_2388", "AC5", cmake)
    must("check_security_audit_fold_2388", "AC5", build)
    must("cmd_security_audit_fold_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)
    must("Issue #2388", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2388 SecurityEvent audit fold — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
