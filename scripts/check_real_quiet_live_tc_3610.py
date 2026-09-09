#!/usr/bin/env python3
# scripts/check_real_quiet_live_tc_3610.py -- Issue #3610 source-cite gate.
#
# AC1: real-Quiet (override<0, depth==0) with a live TLS commit TC bound +
#      latched pending_full_solve_residual face refuses IR/JIT entry
#      (ir_typed_entry_real_quiet_allows + #3305 counter reuse).
# AC2: the no-TLS metrics path keeps the #3568 allow (face not consulted).
# AC3: probe override==0 consume unchanged (#3510 parity).
# AC4: Soft/Off unchanged (production gate still first).
# AC5: drain + green stamp restores the live-TC depth==0 allow.
# AC6: build.py wires this linter; ac3610 rows live in the commit
#      readiness suite; no docs/design/*3610*, no tests/**/test_issue_3610.cpp.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

TMA = "src/compiler/typed_mutation_audit.h"
TEST = "tests/compiler/test_commit_readiness_score.cpp"
BUILD = "build.py"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    _ = ap.parse_args()

    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    tma = _read(TMA)
    test = _read(TEST)
    build = _read(BUILD)

    # AC1
    must("Issue #3610", "AC1 header cite", tma)
    must("kRealQuietLiveTcTypedEntryIssue = 3610", "AC1 stamp", tma)
    must("ir_typed_entry_real_quiet_allows", "AC1 helper", tma)
    must("g_tls_audit_commit_readiness_evaluator == nullptr", "AC1 no-TLS allow", tma)
    must("ac3610_real_quiet_live_tc_face_split", "AC1 test fn", test)
    must("3610 AC1: live commit TC + pending face refuses real Quiet depth==0", "AC1 test check", test)

    # AC2
    must("3610 AC2: no-TLS real Quiet still allows leftover pending face", "AC2 test row", test)

    # AC3
    must("pending_full_solve_residual_face_hit()", "AC3 probe consume kept", tma)
    must("kDepthZeroTypedEntryNegativeAuthorityIssue = 3510", "AC3 3510 kept", tma)
    must("3610 AC3: probe override==0 refuses pending face (3510 parity)", "AC3 test row", test)

    # AC4
    must("production_defaults_active() || get_strategy() == AuditStrategy::Full", "AC4 soft gate first", tma)
    must("3610 AC4: Soft still allows before depth math", "AC4 test row", test)

    # AC5
    must("3610 AC5: drain + green stamp restores live-TC depth==0 allow", "AC5 test row", test)

    # AC6
    must("check_real_quiet_live_tc_3610", "AC6 build wiring", build)
    must("g_linear_fast_path_elide_blocked_production_total", "AC6 counter reuse", tma)
    if (ROOT / "tests" / "compiler" / "test_issue_3610.cpp").is_file():
        fails.append("AC6: test_issue_3610.cpp present (forbidden per #81934/#81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*3610*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3610 real-Quiet live commit TC face split — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
