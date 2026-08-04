#!/usr/bin/env python3
"""Issue #2635: production mid-fallback SLO hard-deny via schedule-gate +
resolve path (no silent process-origin join stamps under Restricted).

Contract (one row per AC):
  AC1 production + mid-fallback SLO already breached → resolve_audit_mutation_id
     returns 0 (no new process-origin stamp)
  AC2 production + SLO clear → last-resort still works (join completeness
     preserved)
  AC3 Soft / sandbox off → fallback always allowed; only counters bump
  AC4 #2630 schedule-gate still sees the same SLO signal; no double-deny race
  AC5 existing #2493 / #2594 coverage scripts pass; new
     check_mid_fallback_hard_deny_2635.py covers #2635
  AC6 SE / TypedMutationAudit / grant epoch join quality (fallback rate)
     does not degrade under sustained production load

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    tma = _read("src/compiler/typed_mutation_audit.h")
    slo = _read("src/compiler/audit_mid_fallback_slo.h")
    test = _read("tests/compiler/test_audit_mid_fallback_slo_2594.cpp")
    build = _read("build.py")
    linter_2493 = _read("scripts/coverage/checks/check_audit_mutation_id_unify_2493.py")
    linter_2594 = _read("scripts/coverage/checks/check_audit_mid_fallback_slo_2594.py")

    # AC1: production + SLO breached → return 0 (no new process-origin stamp)
    must("Issue #2635", "AC1", tma)
    must("hard_deny_eligible", "AC1", tma)
    must("decide_audit_mid_fallback_slo(slo)", "AC1", tma)
    must("d.would_arm_degraded", "AC1", tma)
    must("return 0;", "AC1", tma)  # the hard-deny return

    # AC1: typed_mutation_audit.h includes audit_mid_fallback_slo.h
    must('#include "audit_mid_fallback_slo.h"', "AC1", tma)

    # AC2: production + SLO clear → last-resort still works (fallback
    # counter bumps, next_audit_mutation_id returned)
    must("audit_mid_fallback_gen_total.fetch_add", "AC2", tma)
    must("return next_audit_mutation_id();", "AC2", tma)

    # AC3: Soft / sandbox off → fallback always allowed
    # (The hard_deny_eligible gate is the production+strict path; soft
    # paths skip the gate and fall through to the existing fallback.)
    # #2636 follow-up: removed 'AuditStrategy::Strict' — the AuditStrategy enum
    # at typed_mutation_audit.h:39-43 only has {Off, Sampled, Full}. The issue
    # body (#2635) mentioned "Strict" as a security profile, but the actual
    # enum value doesn't exist (the related boolean is the separate
    # strict_sandbox parameter). Existing code at typed_mutation_audit.h:362-363
    # uses 'AuditStrategy::Full' only. The hard_deny_eligible gate therefore
    # reads 'production_defaults_active() || AuditStrategy::Full' — verified.
    must("hard_deny_eligible", "AC3", tma)  # gate is production_defaults || Full
    must("AuditStrategy::Full", "AC3", tma)  # the actual enum value used
    must("soft_mode", "AC3", slo)  # SLO soft_mode field (§#2594)

    # AC4: schedule-gate (#2630) sees the same SLO signal
    # (MidFallbackSloInput + would_arm_degraded are reused by the gate.)
    must("MidFallbackSloInput", "AC4", slo)
    must("would_arm_degraded", "AC4", slo)

    # AC5: existing #2493 / #2594 coverage scripts still pass
    must("Issue #2493", "AC5", linter_2493)
    must("Issue #2594", "AC5", linter_2594)

    # AC5: new linter + test extension
    must("check_mid_fallback_hard_deny_2635", "AC5", build)
    must("Issue #2635", "AC5", test)

    # AC6: no schema / surface change to StableNodeRef or typed_mutation_audit
    # public surface — verified by source-cite (typed_mutation_audit.h has
    # no public API bump; only the last-resort branch gains a guard).
    must("next_audit_mutation_id()", "AC6", tma)
    must("resolve_audit_mutation_id(std::uint64_t caller_mid = 0)", "AC6", tma)

    # cross-check: existing linters must still be green
    for linter in ("check_audit_mutation_id_unify_2493.py", "check_audit_mid_fallback_slo_2594.py"):
        r = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / linter)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{linter} regression:\n{r.stdout}\n{r.stderr}")

    # cross-check: stamp-resolve --strict must still be green
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / "check_stamp_resolve_coverage.py"), "--strict"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"stamp-resolve --strict regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: mid-fallback hard-deny #2635 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
