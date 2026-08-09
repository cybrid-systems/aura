#!/usr/bin/env python3
"""Issue #2635: production mid-fallback hard-deny lineage (resolve + schedule).

#2836 upgraded the *resolve-time* face to absolute zero-tolerance under
production_defaults || Full (no rate check). This linter preserves the
#2635 contract surface that still holds:

  AC1 production / Full last-resort → resolve returns 0 (hard_deny_eligible)
  AC2 Soft / Sampled last-resort still works (gen counter + next mid)
  AC3 Soft path skips hard_deny_eligible (Sampled / not Full)
  AC4 #2630 schedule-gate still sees MidFallbackSloInput / would_arm_degraded
  AC5 existing #2493 / #2594 coverage scripts pass; this linter stays wired
  AC6 resolve signature + next_audit_mutation_id Soft path preserved

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
    test = _read("tests/compiler/test_audit_mid_fallback_slo.cpp")
    build = _read("build.py")
    linter_2493 = _read("scripts/coverage/checks/check_audit_mutation_id_unify_2493.py")
    linter_2594 = _read("scripts/coverage/checks/check_audit_mid_fallback_slo_2594.py")

    # AC1: production/Full hard-deny face → return 0 (absolute after #2836)
    must("#2635", "AC1", tma)
    must("hard_deny_eligible", "AC1", tma)
    must("return 0;", "AC1", tma)
    must("production_defaults_active()", "AC1", tma)

    # AC2: Soft last-resort still present (gen bump + next mid)
    must("audit_mid_fallback_gen_total.fetch_add", "AC2", tma)
    must("return next_audit_mutation_id();", "AC2", tma)

    # AC3: Full strategy arm; Soft skips gate
    must("hard_deny_eligible", "AC3", tma)
    must("AuditStrategy::Full", "AC3", tma)
    must("soft_mode", "AC3", slo)

    # AC4: schedule-gate (#2630) still sees SLO signal (admission face)
    must("MidFallbackSloInput", "AC4", slo)
    must("would_arm_degraded", "AC4", slo)

    # AC5: existing #2493 / #2594 coverage scripts still present
    must("Issue #2493", "AC5", linter_2493)
    must("Issue #2594", "AC5", linter_2594)
    must("check_mid_fallback_hard_deny_2635", "AC5", build)
    must("#2635", "AC5", test)

    # AC6: resolve surface unchanged; Soft next_audit still called
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
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_stamp_resolve_coverage.py"),
            "--strict",
        ],
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
