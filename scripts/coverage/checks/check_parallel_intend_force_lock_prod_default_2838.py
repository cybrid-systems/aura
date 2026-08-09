#!/usr/bin/env python3
"""Issue #2838: production default enable parallel_intend_force_lock_on_violation.

Contract (one row per AC):
  AC1 production_defaults + flag false → effective force-lock on; Soft/dev_off off
  AC2 env AURA_PARALLEL_INTEND_FORCE_LOCK=0 opt-out under production
  AC3 additive counter + query:orch-module-stats schema-2838; #2662 keys preserved
  AC4 README production-default wording; no transactional isolation claim
  AC5 extend test_parallel_intend_pure_contract + linter; no invent file
  AC6 zero cost on :pure #f (resolve gated on pure_mode)

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

    spawn = _read("src/orch/agent_spawn.h")
    ag = _read("src/compiler/evaluator_primitives_agent.cpp")
    rd = _read("src/orch/README.md")
    test = _read("tests/orch/test_parallel_intend_pure_contract.cpp")
    build = _read("build.py")

    # AC1 — resolve helper + production default inject
    must("Issue #2838", "AC1", spawn)
    must("resolve_parallel_intend_force_lock_on_violation", "AC1", spawn)
    must("ParallelIntendForceLockDecision", "AC1", spawn)
    must("default_applied", "AC1", spawn)
    must("force_lock_on_violation_policy", "AC1", ag)
    must("production_defaults", "AC1", spawn)

    # AC2 — env opt-out
    must("AURA_PARALLEL_INTEND_FORCE_LOCK", "AC2", spawn)
    must("parallel_intend_force_lock_env_pref", "AC2", spawn)
    must("AURA_PARALLEL_INTEND_FORCE_LOCK=0", "AC2", rd)

    # AC3 — counter + query surface; #2662 preserved
    must("parallel_intend_force_lock_default_applied_total", "AC3", spawn)
    must("parallel-intend-force-lock-default-applied-total", "AC3", ag)
    must("schema-2838", "AC3", ag)
    must("issue-2838", "AC3", ag)
    must("schema-2662", "AC3", ag)
    must("parallel_intend_force_lock_on_violation", "AC3", spawn)

    # AC4 — README production default; no transactional isolation
    must("#2838", "AC4", rd)
    must("production default", "AC4", rd)
    must("best-effort", "AC4", rd)
    # Forbid advertising transactional isolation on this path.
    # Allowed only in the forbid sentence ("Do not advertise …").
    if (
        "transactional isolation level" in rd.lower()
        and ":pure #t" in rd
        and "as a transactional isolation" in rd
        and "Do not advertise" not in rd
    ):
        fails.append("AC4: README must not advertise transactional isolation")

    # AC5 — test extension + linter wire + no invent
    must("2838 AC1", "AC5", test)
    must("resolve_parallel_intend_force_lock_on_violation", "AC5", test)
    must("check_parallel_intend_force_lock_prod_default_2838", "AC5", build)
    if (ROOT / "tests" / "orch" / "test_issue_2838.cpp").is_file():
        fails.append("AC5: test_issue_2838.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2838-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    # AC6 — pure_mode gate
    must("if (pure_mode)", "AC6", ag)
    must("resolve_parallel_intend_force_lock_on_violation", "AC6", ag)

    # Cross-check: #2662 + pure-isolation wording still green
    for linter, args in (
        ("check_2662_coverage.py", []),
        ("check_pure_parallel_isolation_wording.py", ["--self-test"]),
    ):
        r = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / linter), *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{linter} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2838 parallel-intend force-lock production default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
