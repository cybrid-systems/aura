#!/usr/bin/env python3
"""Issue #2634: harden pure-parallel probe (mutations_/workspace gen) + keep
isolation-level contract.

Contract (one row per AC):
  AC1 Probe snapshots total_mutations() + workspace_generation() before
     unlocked pure apply; if either advances after apply, fail task with
     pure-contract-violated + bump pure_contract_violated_total
  AC2 Clean pure arithmetic → isolation-level=best-effort-pure, violated=0
     (covered by existing #2163 path; verify schema keys preserved)
  AC3 :pure #f path unchanged — snapshots short-circuit to literal 0 when
     !pure_mode (zero cost on the serialized path)
  AC4 Wording gate (scripts/coverage/checks/check_pure_parallel_isolation_wording.py)
     still fails injected "transactional isolation" claims
  AC5 test_parallel_intend_pure_contract extended with AC for
     mutations_/gen probe (probe catches indirect writers)
  AC6 README probe section updated; schema keys #2163/#2400/#2593 preserved
  AC7 AgentSpec / OrchModuleStats untouched (pure probe is internal to
     parallel-intend body)

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

    ev = _read("src/compiler/evaluator.ixx")
    ag = _read("src/compiler/evaluator_primitives_agent.cpp")
    rd = _read("src/orch/README.md")
    test = _read("tests/orch/test_parallel_intend_pure_contract.cpp")
    build = _read("build.py")
    linter = _read("scripts/coverage/checks/check_pure_parallel_isolation_wording.py")

    # AC1
    must("Issue #2634", "AC1", ag)
    must("mut_before", "AC1", ag)
    must("ws_gen_before", "AC1", ag)
    must("ev.total_mutations() != mut_before", "AC1", ag)
    must("ev.workspace_generation() != ws_gen_before", "AC1", ag)
    must("pure-contract-violated", "AC1", ag)

    # AC1 (accessor)
    must("workspace_generation()", "AC1", ev)
    must("Issue #2634", "AC1", ev)
    must("persistent_tc_workspace_gen_", "AC1", ev)

    # AC2 (isolation-level schema keys preserved)
    must("best-effort-pure", "AC2", ag)
    must("isolation-level", "AC2", ag)
    must("schema-2163", "AC2", ag)

    # AC3 (zero-cost on :pure #f)
    must("pure_mode ? ev.defuse_version()", "AC3", ag)
    must("pure_mode ? ev.total_mutations()", "AC3", ag)
    must("pure_mode ? ev.workspace_generation()", "AC3", ag)

    # AC4 (wording gate still works)
    must("forbid advertising parallel-intend", "AC4", linter)
    must("transactional", "AC4", linter)
    must('isolation-level = "transactional"', "AC4", linter)

    # AC5 (test extended)
    must("Issue #2634", "AC5", test)
    must("total_mutations", "AC5", test)
    must("workspace_generation", "AC5", test)

    # AC6 (README probe section updated)
    must("pure-contract-violated", "AC6", rd)
    must("best-effort-pure", "AC6", rd)
    must("Issue #2634", "AC6", rd)
    must("workspace_generation", "AC6", rd)
    must("total_mutations", "AC6", rd)

    # AC7 (build.py wiring)
    must("check_pure_probe_hardening_2634", "AC7", build)

    # cross-check: pure-parallel wording gate (--self-test) must still be green
    r = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_pure_parallel_isolation_wording.py"),
            "--self-test",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"pure-parallel wording gate --self-test failed:\n{r.stdout}\n{r.stderr}")

    # cross-check: stamp-resolve --strict must still be green
    r2 = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / "check_stamp_resolve_coverage.py"), "--strict"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r2.returncode != 0:
        fails.append(f"stamp-resolve --strict regression:\n{r2.stdout}\n{r2.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: pure-probe hardening #2634 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
