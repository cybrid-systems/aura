#!/usr/bin/env python3
"""Issue #2917: closed-loop agent:recover-from-error + recovery stats.

AC:
  1. agent:recover-from-error registered under MutationBoundaryGuard
  2. diagnose extended for quota / type classes
  3. query:agent-recovery-stats facade + catalog + metrics fields
  4. Docs + suite + build.py wiring
  5. Side-effect markers (agent: → Mutate / AURA_SIDE_EFFECT_PRIM)
"""

from __future__ import annotations

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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    diag = _read("src/compiler/evaluator_primitives_diagnostic.cpp")
    must("Issue #2917" in diag, "AC1: diagnostic cites #2917")
    must('add("agent:recover-from-error"' in diag, "AC1: recover prim registered")
    must("MutationBoundaryGuard::try_acquire" in diag, "AC1: Guard on recovery")
    must("save_panic_checkpoint" in diag, "AC1: panic checkpoint")
    must("clear_last_mutate_error" in diag, "AC1: clears hold / last error")
    must("note_agent_recovery" in diag, "AC1: recovery metrics hooks")
    must("prim-heap-quota" in diag, "AC2: diagnose quota class")
    must("AURA_SIDE_EFFECT_PRIM" in diag, "AC5: side-effect marker")
    must("security_side_effect.hh" in diag, "AC5: side-effect header")

    ixx = _read("src/compiler/evaluator.ixx")
    must("note_agent_recovery_attempt" in ixx, "AC1: Evaluator recovery APIs")
    must("agent_recovery_attempts_total_" in ixx, "AC1: recovery counters")

    metrics = _read("src/compiler/observability_metrics.h")
    must("agent_recovery_attempts_total" in metrics, "AC3: CompilerMetrics fields")
    must("2917" in metrics, "AC3: metrics cite 2917")

    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    must("query:agent-recovery-stats" in obs, "AC3: stats surface")
    must("success-rate-bp" in obs, "AC3: success rate field")
    must("schema" in obs and "2917" in obs, "AC3: schema 2917")

    cat = _read("src/compiler/evaluator_primitives_observability.cpp")
    must("query:agent-recovery-stats" in cat, "AC3: stats catalog")

    doc = _read("docs/stdlib/agent-recovery.md")
    must("2917" in doc and "agent:recover-from-error" in doc, "AC4: docs")
    must("query:agent-recovery-stats" in doc, "AC4: docs stats")

    contrib = _read("docs/contributing.md")
    must("agent-recovery" in contrib or "2917" in contrib, "AC4: contributing pointer")

    suite = _read("tests/suite/agent_recovery_2917.aura")
    must("agent:recover-from-error" in suite, "AC4: suite prim")
    must("query:agent-recovery-stats" in suite, "AC4: suite stats")
    must("unbound variable: map" in suite, "AC4: unbound failure class")

    build = _read("build.py")
    must("agent-recovery-2917" in build or "agent_recovery_2917" in build, "AC4: build.py")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2917 agent recovery closed loop — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
