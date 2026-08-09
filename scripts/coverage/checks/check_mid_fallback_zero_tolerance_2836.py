#!/usr/bin/env python3
"""Issue #2836: production mid-fallback absolute zero-tolerance.

Contract (one row per AC):
  AC1 production_defaults / Full + all upstream mid=0 → resolve returns 0;
      audit_mid_fallback_gen_total does not bump; refuse counter does
  AC2 Soft / Sampled + mid=0 → still generates join stamp + gen counter
  AC3 non-zero caller / epoch / RQ mid preference unchanged
  AC4 Agent-visible mid-fallback-refused / refused-total metric
  AC5 Additive query keys + schema-2836; #2493/#2635 surfaces preserved
  AC6 Source-cite + coverage linter; extend audit mid test; no docs/design/

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
    sec = _read("src/compiler/evaluator_primitives_security.cpp")
    test = _read("tests/compiler/test_audit_mid_fallback_slo.cpp")
    unify = _read("tests/compiler/test_audit_mutation_id_unify.cpp")
    build = _read("build.py")
    linter_2635 = _read("scripts/coverage/checks/check_mid_fallback_hard_deny_2635.py")

    # AC1 — absolute refuse under production/Full
    must("Issue #2836", "AC1", tma)
    must("hard_deny_eligible", "AC1", tma)
    must("audit_mid_fallback_refused_total", "AC1", tma)
    must("return 0;", "AC1", tma)
    must("production_defaults_active() || get_strategy() == AuditStrategy::Full", "AC1", tma)
    # Soft gen path still present after hard branch
    must("audit_mid_fallback_gen_total.fetch_add", "AC1", tma)
    must("return next_audit_mutation_id();", "AC1", tma)

    # AC2 — Soft last-resort preserved
    must("apply_dev_audit_defaults", "AC2", test)
    must("2836 AC2", "AC2", test)

    # AC3 — preference order comments / non-zero caller path
    must("caller_mid != 0", "AC3", tma)
    must("current_mutation_epoch()", "AC3", tma)
    must("2836 AC3", "AC3", test)

    # AC4 — Agent-visible refuse surface
    must("mid-fallback-refused", "AC4", sec)
    must("refused-total", "AC4", sec)
    must("audit_mid_fallback_refused_total", "AC4", sec)

    # AC5 — schema + preserved surfaces
    must("schema-2836", "AC5", sec)
    must("issue-2836", "AC5", sec)
    must("zero-tolerance-wired", "AC5", sec)
    must("schema-2594", "AC5", sec)
    must("check_mid_fallback_hard_deny_2635", "AC5", build)
    must("check_mid_fallback_hard_deny_2635", "AC5", linter_2635)

    # AC6 — test extension + linter wire; no invent file; no design docs
    must("2836", "AC6", test)
    must("check_mid_fallback_zero_tolerance_2836", "AC6", build)
    must("apply_dev_audit_defaults", "AC6", unify)  # Soft AC4 fix under Full default
    if (ROOT / "tests" / "compiler" / "test_issue_2836.cpp").is_file():
        fails.append("AC6: test_issue_2836.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2836-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: #2635 linter still green after absolute-refuse reshape
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / "check_mid_fallback_hard_deny_2635.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"check_mid_fallback_hard_deny_2635 regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: mid-fallback absolute zero-tolerance #2836 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
