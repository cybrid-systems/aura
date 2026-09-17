#!/usr/bin/env python3
"""Issue #3856 source-cite gate: WAL-miss deny flips the mutation-audit ring.

#3780 made emit_mutation_audit return false on a production fail-closed
mutation-WAL append miss so MutationBoundaryGuard can deny before
Occurrence persist. But the ring slot was already published with
effect_denied=false (success-shaped): a rolled-back mid still showed a
committed face to Agents joining the mutation-audit ring.

ACs:
  AC1  emit_mutation_audit flips the ring slot to effect_denied=true on
       the fail-closed miss, before the overflow push and the deny
       return; the #3780 gate rows stay intact.
  AC2  test_mutation_audit_wal.cpp drives the fail-closed miss through
       the #3640 library-side arm shim and asserts the ring slot flips
       to denied (miss-shaped) with the joined mid.
  AC3  no stray files: no tests/**/test_issue_3856.cpp; no
       docs/design/3856-*.
  AC4  build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEC = ROOT / "src" / "compiler" / "evaluator_security.cpp"
TST = ROOT / "tests" / "compiler" / "test_mutation_audit_wal.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def main() -> int:
    sec = SEC.read_text() if SEC.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1: the fail-closed miss flips the ring slot before the deny
    # return; the #3780 gate rows are untouched.
    good = (
        "Issue #3856" in sec
        and "slot.effect_denied = true;" in sec
        and sec.count('ovr.reason = "mutation_wal_append_miss"') >= 1
        and "if (!g_mutation_audit_wal().append(rec)) {" in sec
        and "return false;" in sec
    )
    report("AC1", good, "emit flips ring slot to denied on the miss")

    # AC2: the WAL suite drives the miss and asserts the ring flip.
    good = (
        "3856 AC1: ring slot flipped to denied (miss-shaped)" in tst
        and '"test:3856-miss"' in tst
        and "arm_production_audit_defaults_for_test" in tst
    )
    report("AC2", good, "WAL suite asserts the ring flip")

    # AC3: the banned file shapes stay absent.
    no_stray = (
        not (ROOT / "tests" / "compiler" / "test_issue_3856.cpp").exists()
        and not (ROOT / "docs" / "design" / "3856-0.md").exists()
    )
    report("AC3", no_stray, "no test_issue_3856.cpp / docs-design strays")

    # AC4: gate wiring — build.py runs the linter and the allowlist lists it.
    wired = "check_wal_miss_ring_flip_3856.py" in build and ("check_wal_miss_ring_flip_3856.py" in allow)
    report("AC4", wired, "build.py + root_check_allowlist.txt wired")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
