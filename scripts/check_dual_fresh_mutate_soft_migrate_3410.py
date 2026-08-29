#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #3410: Production mutate dual-fresh miss must not soft-migrate
# onto pre-mutate g_jit_fns native.
#
# AC1 — try_cross_cow_soft_migrate_ has production probe (aura::compiler::
#       typed_audit::production_defaults_active) AFTER within-cap check
#       and BEFORE the restamp path (same-gen drift → MustDeopt + return 0).
# AC2 — Production probe sets g_closure_must_deopt[cid] = 1 + cross_cow_note_hard_
#       (CrossCowHardReject::Other — reuse existing reason per AC5).
# AC3 — Soft/Off path retained: production probe == 0 → existing restamp.
# AC4 — True COW catch-up (#2547) unchanged: cow_gen_mismatch hard-rejects above.
# AC5 — No new metric field in observability_metrics.h; reuses
#       MustDeopt + cross_cow_hard_reject_total counters.
# AC6 — No docs/design/3410-*.md (banned per #1655) and no
#        tests/{issues,compiler,core}/test_issue_3410.cpp (must extend
#        test_cross_cow_soft_migrate.cpp per #81934).
# AC7 — test_cross_cow_soft_migrate.cpp carries AC6/AC7/AC8 markers for #3410.
# AC7 — build.py registers check_dual_fresh_mutate_soft_migrate_3410.
#
# Self-test:
#   python3 scripts/check_dual_fresh_mutate_soft_migrate_3410.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    fails: list[str] = []

    rt = (ROOT / "src" / "compiler" / "aura_jit_runtime.cpp").read_text()
    hot = (ROOT / "src" / "compiler" / "hot_update_registry.cpp").read_text()
    obs = (ROOT / "src" / "compiler" / "observability_metrics.h").read_text()
    test = (ROOT / "tests" / "compiler" / "test_cross_cow_soft_migrate.cpp").read_text()
    build = (ROOT / "build.py").read_text()

    # AC1 — production probe between within-cap check and restamp.
    # `production_defaults_active()` is referenced multiple times in the file
    # (line 240/255 existing remount-hard logic), so anchor on the #3410
    # marker and find the production probe call that follows it.
    if "Issue #3410" not in rt:
        fails.append("AC1: aura_jit_runtime.cpp missing 'Issue #3410' marker")
    marker_pos = rt.find("Issue #3410")
    within_cap_pos = rt.find("cross_cow_drift_within_cap_")
    restamp_pos = rt.find("stamp_closure_provenance_locked(cid);")
    if marker_pos < 0:
        fails.append("AC1: aura_jit_runtime.cpp does not contain 'Issue #3410' marker")
    if within_cap_pos < 0:
        fails.append("AC1: aura_jit_runtime.cpp missing within-cap check reference")
    if restamp_pos < 0:
        fails.append("AC1: aura_jit_runtime.cpp missing restamp call site reference")
    if marker_pos > 0 and within_cap_pos > 0 and restamp_pos > 0:
        # Find the production_defaults_active() call that lives AFTER the
        # #3410 marker (the new code path), then verify it sits between
        # within-cap check and restamp.
        prod_pos = rt.find("production_defaults_active()", marker_pos)
        if prod_pos < 0:
            fails.append("AC1: aura_jit_runtime.cpp #3410 block missing production_defaults_active() call")
        elif not (within_cap_pos < prod_pos < restamp_pos):
            fails.append(
                "AC1: #3410 production probe must sit BETWEEN within-cap check "
                "and restamp call site (so cap-drift path unchanged and "
                "restamp cannot be reached on production same-gen drift)"
            )

    # AC2 — MustDeopt set + reuse CrossCowHardReject::Other.
    if "g_closure_must_deopt[cid] = 1" not in rt:
        fails.append("AC2: aura_jit_runtime.cpp missing MustDeopt set on production drift refuse")
    if "cross_cow_note_hard_(CrossCowHardReject::Other)" not in rt:
        fails.append("AC2: aura_jit_runtime.cpp must reuse CrossCowHardReject::Other reason")

    # AC3 — Soft/Off retains restamp path.
    if "stamp_closure_provenance_locked" not in rt:
        fails.append("AC3: aura_jit_runtime.cpp missing stamp_closure_provenance_locked (Soft/Off restamp path)")
    if "aura_bump_cross_cow_soft_migrate_same_gen_total" not in rt:
        fails.append("AC3: aura_jit_runtime.cpp missing same-gen soft-migrate counter bump (Soft/Off path)")

    # AC4 — True COW catch-up unchanged.
    if "closure_cow_gen_mismatch_" not in rt:
        fails.append("AC4: aura_jit_runtime.cpp missing closure_cow_gen_mismatch_ (#2547 unchanged)")
    if "CowGenMismatch" not in rt:
        fails.append("AC4: aura_jit_runtime.cpp missing CowGenMismatch reason")
    if "hard_invalidate_via_facade" not in rt and "hard_invalidate_via_facade" not in hot:
        fails.append("AC4: hard_invalidate_via_facade reference missing")

    # AC5 — No new metric field (reuse MustDeopt + cross_cow_hard_reject_total).
    if "dual_fresh_mutate_soft_migrate" in obs or "3410_soft_migrate" in obs:
        fails.append("AC5: observability_metrics.h contains new #3410 metric field (forbidden — reuse existing)")
    if "cross_cow_hard_reject_total" not in obs:
        fails.append("AC5: observability_metrics.h missing cross_cow_hard_reject_total counter (must pre-exist)")

    # AC6 — No docs/design/3410-*.md; no test_issue_3410.cpp.
    if list((ROOT / "docs" / "design").glob("3410-*.md")):
        fails.append("AC6: docs/design/3410-*.md exists — design docs banned per #1655")
    for sub in ("issues", "compiler", "core"):
        if (ROOT / "tests" / sub / "test_issue_3410.cpp").is_file():
            fails.append(
                f"AC6: tests/{sub}/test_issue_3410.cpp exists — must extend existing "
                "test_cross_cow_soft_migrate.cpp per #81934"
            )

    # AC7 — test markers.
    if "AC6: #3410 production probe" not in test:
        fails.append("AC7: test_cross_cow_soft_migrate.cpp missing AC6 for #3410 (production probe)")
    if "AC7: #3410 no docs/design/3410-*" not in test:
        fails.append("AC7: test_cross_cow_soft_migrate.cpp missing AC7 for #3410 (no design docs)")
    if "AC8: #3410 build.py wiring + no new metric" not in test:
        fails.append("AC7: test_cross_cow_soft_migrate.cpp missing AC8 for #3410 (build wiring + no new metric)")

    # AC7 — build.py wiring.
    if "cmd_dual_fresh_mutate_soft_migrate_3410_coverage" not in build:
        fails.append("AC7: build.py does not register cmd_dual_fresh_mutate_soft_migrate_3410_coverage")
    if "check_dual_fresh_mutate_soft_migrate_3410" not in build:
        fails.append("AC7: build.py does not register check_dual_fresh_mutate_soft_migrate_3410 linter script")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1

    print("PASS: #3410 production mutate dual-fresh soft-migrate refuse contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
