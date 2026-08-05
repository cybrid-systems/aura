#!/usr/bin/env python3
"""Issue #2662: production hardening of pure-parallel path under multi-agent
fanout. Coverage gate — locks the new contract surface.

Contract (one row per AC):
  AC1 OrchModuleStats declares parallel_intend_force_lock_on_violation
     atomic<bool> (default false) — opt-in flag for production hardening.
  AC2 evaluator_primitives_agent.cpp AuraShared struct declares the per-batch
     batch_force_eval_mu atomic<bool> (default false) — set on violation,
     read at task start to force-lock the rest of the batch.
  AC3 violation branch wires production_defaults_active() &&
     parallel_intend_force_lock_on_violation.load() → batch_force_eval_mu.store(true).
     Gates the wire-up on production defaults + opt-in flag.
  AC4 force_lock calc reads batch_force_eval_mu.load() so subsequent pure
     tasks in the same batch take eval_mu (pure_fallback_locked bumps).
  AC5 src/orch/README.md documents #2662 production hardening + #2651
     heap-race precedent (string_heap / pairs races under fanout). Keeps
     'best-effort' disclaimer (no transactional claim).
  AC6 tests/orch/test_parallel_intend_pure_contract.cpp extended with #2662
     AC6 (flag atomic + wire-up source-cite) + AC7 (8+ fibers string/cons
     stress under :pure #t — no SIGSEGV invariant under fanout).
  AC7 build.py wires check_2662_coverage into the gate (runs before
     pre-push); cross-check: check_pure_probe_hardening_2634 +
     check_pure_parallel_isolation_wording still green.

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

    spawn_src = _read("src/orch/agent_spawn.h")
    ag = _read("src/compiler/evaluator_primitives_agent.cpp")
    rd = _read("src/orch/README.md")
    test = _read("tests/orch/test_parallel_intend_pure_contract.cpp")
    bp = _read("build.py")

    # AC1 — OrchModuleStats flag atomic
    must("parallel_intend_force_lock_on_violation", "AC1", spawn_src)
    must("std::atomic<bool>", "AC1", spawn_src)
    must("Issue #2662", "AC1", spawn_src)

    # AC2 — per-batch batch_force_eval_mu atomic in AuraShared
    must("batch_force_eval_mu", "AC2", ag)
    must("std::atomic<bool> batch_force_eval_mu", "AC2", ag)
    must("Issue #2662", "AC2", ag)

    # AC3 — violation branch wires production_defaults + flag → batch_force_eval_mu.store(true)
    must("parallel_intend_force_lock_on_violation", "AC3", ag)
    must("batch_force_eval_mu.store(true", "AC3", ag)
    must("production_defaults_active()", "AC3", ag)
    must("Issue #2662", "AC3", ag)

    # AC4 — force_lock calc reads batch_force_eval_mu.load()
    must("ash->batch_force_eval_mu.load(std::memory_order_relaxed)", "AC4", ag)

    # AC5 — README documents #2662 production hardening + #2651 heap-race precedent
    must("Issue #2662", "AC5", rd)
    must("parallel_intend_force_lock_on_violation", "AC5", rd)
    must("Issue #2651", "AC5", rd)
    must("best-effort", "AC5", rd)
    must("batch_force_eval_mu", "AC5", rd)

    # AC6 — test file extended with #2662 ACs + 8+ fibers stress
    must("2662 AC6", "AC6", test)
    must("2662 AC7", "AC6", test)
    must("parallel_intend_force_lock_on_violation", "AC6", test)
    must("batch_force_eval_mu.store(true", "AC6", test)
    must(":max-concurrency 8", "AC6", test)
    must("string-append", "AC6", test)
    must("(cons 1 2)", "AC6", test)

    # AC7 — build.py wires check_2662_coverage into the gate
    must("check_2662_coverage", "AC7", bp)

    # Cross-check: check_pure_probe_hardening_2634 still green
    r1 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_pure_probe_hardening_2634.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r1.returncode != 0:
        fails.append(f"check_pure_probe_hardening_2634 regression:\n{r1.stdout}\n{r1.stderr}")

    # Cross-check: check_pure_parallel_isolation_wording --self-test still green
    r2 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_pure_parallel_isolation_wording.py"),
            "--self-test",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r2.returncode != 0:
        fails.append(f"check_pure_parallel_isolation_wording --self-test regression:\n{r2.stdout}\n{r2.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2662 production hardening + chaos stress coverage — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
