#!/usr/bin/env python3
"""Issue #3854 source-cite gate: fiber-scoped session revoke stamps fiber on
SE/audit evidence.

#3241/#3799 made fiber-scoped session revoke privilege-correct: grants are
filtered by (mid, fiber_id) and peer fibers are skipped. But the SE/audit
dual-write built `audit_prov` without `fiber_id`, so every revoke row read
`fiber_id=0` — agents joining SE by (mid, fiber) for revoke attribution
could not tell which fiber's session grants died. Same-shape sites: the
mid-revoke sweep, the TenantScope scope-dtor cascade, and the orphan sweep.
MED (mid-revoke only): `ep == 0 -> 1` invented an epoch under the hard face,
beside the #3844 grant/require contract where 0 stays 0.

ACs:
  AC1  all three session-revoke paths stamp `audit_prov.fiber_id = fiber_id`
       (mid-revoke, scope-dtor cascade, orphan sweep).
  AC2  the mid-revoke epoch routes through capability_epoch_hard_face():
       hard face keeps Mutation epoch 0 as 0; Soft keeps the observe invent.
  AC3  tests/core/test_capability_single_use_consume.cpp extends the #2944
       suite with ac3854_* ACs asserting SE fiber joins via ring_lookup_reason;
       no test_issue_3854.cpp; no docs/design/3854-*.
  AC4  build.py wires this linter and scripts/coverage/root_check_allowlist.txt
       lists it.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "src" / "core" / "capability_model.hh"
TST = ROOT / "tests" / "core" / "test_capability_single_use_consume.cpp"
BUILD = ROOT / "build.py"
ALLOWLIST = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def main() -> int:
    core = CORE.read_text() if CORE.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = BUILD.read_text() if BUILD.exists() else ""
    allow = ALLOWLIST.read_text() if ALLOWLIST.exists() else ""
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # AC1: all three fiber-filtered session-revoke paths stamp the caller
    # fiber on the SE/audit provenance.
    stamps = core.count("audit_prov.fiber_id = fiber_id;")
    report("AC1", stamps >= 3, f"revoke paths stamp caller fiber (found {stamps}, need >=3)")

    # AC2: the mid-revoke epoch joins through the hard-face gate — 0 stays 0
    # under production (the #3844 contract), Soft keeps the observe invent.
    good = (
        "Issue #3854: hard face keeps the #3844 contract" in core
        and "capability_epoch_hard_face() ? ::aura::core::current_mutation_epoch() : ep" in core
    )
    report("AC2", good, "mid-revoke epoch routes through capability_epoch_hard_face")

    # AC3: the single-use consume suite asserts SE fiber joins; the banned
    # file shapes stay absent.
    good = (
        "Issue #3854" in tst
        and "ac3854_1_revoke_se_carries_caller_fiber" in tst
        and "ac3854_2_legacy_zero_fiber_rows_unchanged" in tst
        and "SE fiber_id == caller fiber (not 0)" in tst
        and 'ring_lookup_reason("session-mid-exit")' in tst
    )
    no_stray = (
        not (ROOT / "tests" / "core" / "test_issue_3854.cpp").exists()
        and not (ROOT / "docs" / "design" / "3854-0.md").exists()
    )
    report("AC3", good and no_stray, "suite asserts SE fiber joins; no stray files")

    # AC4: gate wiring — build.py runs the linter and the allowlist lists it.
    wired = "check_mse_revoke_fiber_3854.py" in build and "check_mse_revoke_fiber_3854.py" in allow
    report("AC4", wired, "build.py + root_check_allowlist.txt wired")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
