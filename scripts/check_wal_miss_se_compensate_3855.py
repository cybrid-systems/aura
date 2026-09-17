#!/usr/bin/env python3
"""Issue #3855 source-cite gate: WAL-miss deny compensates the SE/ring face.

#3639 made a mutation-WAL append miss under fail-closed deny the same
mutate (zero side effect). But the capability dual-write had already run
as EffectAllow and the mutation-audit ring slot read effect_denied=false:
forensics saw "allowed but never audited as success" for a mid the
require_effect actually denied, with no matching Typed Success (the early
return skips capture_security_correlated_audit).

ACs:
  AC1  check_and_record_effect compensates on the fail-closed miss:
       the ring slot flips to effect_denied=true and a compensating
       EffectDeny SE with reason mutation_wal_append_miss is emitted
       before the early return; the #3639 append gate is intact.
  AC2  test_security_posture_trail.cpp asserts the compensating EffectDeny
       row (reason mutation_wal_append_miss) and that no compensator row
       is misfiled as Allow.
  AC3  no stray files: no tests/**/test_issue_3855.cpp; no
       docs/design/3855-*.
  AC4  build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EV = ROOT / "src" / "compiler" / "evaluator_security.cpp"
TST = ROOT / "tests" / "compiler" / "test_security_posture_trail.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def main() -> int:
    ev = EV.read_text() if EV.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1: the fail-closed miss compensates the SE/ring face before the
    # early return; the #3639 append gate is untouched.
    good = (
        "Issue #3855" in ev
        and "slot.effect_denied = true;" in ev
        and ev.count('"mutation_wal_append_miss"') >= 2
        and "if (!g_mutation_audit_wal().append(rec))" in ev
        and "wal_append_missed" in ev
    )
    report("AC1", good, "deny compensates ring slot + EffectDeny SE (miss reason)")

    # AC2: the posture suite asserts the compensating row and forbids a
    # compensator misfiled as Allow.
    good = (
        "3855 AC1: compensating EffectDeny SE (miss reason)" in tst
        and "3855 AC1: no compensator misfiled as Allow" in tst
        and '"test:3855-ac1"' in tst
    )
    report("AC2", good, "posture suite asserts the SE compensation")

    # AC3: the banned file shapes stay absent.
    no_stray = (
        not (ROOT / "tests" / "compiler" / "test_issue_3855.cpp").exists()
        and not (ROOT / "docs" / "design" / "3855-0.md").exists()
    )
    report("AC3", no_stray, "no test_issue_3855.cpp / docs-design strays")

    # AC4: gate wiring — build.py runs the linter and the allowlist lists it.
    wired = "check_wal_miss_se_compensate_3855.py" in build and ("check_wal_miss_se_compensate_3855.py" in allow)
    report("AC4", wired, "build.py + root_check_allowlist.txt wired")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
