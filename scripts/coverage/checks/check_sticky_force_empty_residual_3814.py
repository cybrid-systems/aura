#!/usr/bin/env python3
"""Issue #3814: sticky force-JIT when residual empty — FallBackJit
observe-only + production ResidualForceHeal age belt / Agent-binding.

Contract (one row per AC):
  AC1  Playbook FallBackJit remains observe-only (no auto execution)
  AC2  Production: force!=0 && residual==0 has bounded heal face beyond
       SplitBatch (observe ages FallBackJit; orch RequireAgentRepromote)
  AC3  Soft/Off: zero extra (production_defaults early-return retained)
  AC4  Soak: sticky force without residual leaves demotion via documented
       belt (no undocumented Agent ritual) — tests + cite
  AC5  No invent / docs/design; extend existing tests (#81967)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    # Whitespace-normalized: pins must survive clang-format reflows of the
    # scanned sources (the gate runs format + these checks in one pass).
    p = ROOT / rel
    if not p.is_file():
        return ""
    return " ".join(p.read_text(encoding="utf-8", errors="replace").split())


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    cpp = _read("src/compiler/hot_update_registry.cpp")
    hh = _read("src/compiler/hot_update_registry.hh")
    health = _read("src/compiler/aot_hot_update_health.hh")
    thr_test = _read("tests/compiler/test_orch_hot_update_health_throttle.cpp")
    heal_test = _read("tests/compiler/test_issue_3096.cpp")
    build = _read("build.py")

    # AC1: playbook observe-only — FallBackJit row + no auto execution in decide
    must("Issue #3814", "AC1 playbook cite", cpp)
    must("fall-back-jit", "AC1 playbook row", cpp)
    decide = cpp.find("aura_reload_recovery_playbook_decide")
    if decide < 0:
        fails.append("AC1: decide missing")
    else:
        dbody = cpp[decide : decide + 4500]
        if "aura_reemit_aot_for_dirty" in dbody:
            fails.append("AC1: decide calls reemit (playbook must stay observe-only)")
        if "FallBackJit" not in dbody:
            fails.append("AC1: FallBackJit action missing")
        if (
            "Observe-only" not in dbody
            and "observe-only" not in dbody.lower()
            # decide header comment is above; accept #3814 observe cite nearby
            and "Observe-only (#2953)" not in dbody
        ):
            fails.append("AC1: FallBackJit observe-only cite missing near decide")

    # AC2: bounded heal face + Agent-binding beyond SplitBatch
    must("Issue #3814", "AC2 observe cite", cpp)
    obs = cpp.find("void HotUpdateRegistry::observe_residual_force_stale()")
    if obs < 0:
        fails.append("AC2: observe_residual_force_stale missing")
    else:
        obody = cpp[obs : obs + 4500]
        if "FallBackJit" not in obody and "force sticky" not in obody:
            fails.append("AC2: observe missing FallBackJit / sticky face")
        if "force_jit_regions_mask_.store(0" not in obody:
            fails.append("AC2: observe missing covered demotion clear")
        # Soft gate retained
        if "aura_production_defaults_active_probe() == 0" not in obody:
            fails.append("AC2: Soft early-return missing in observe")

    must("RequireAgentRepromote", "AC2 throttle action", health)
    must("require-agent-repromote", "AC2 action_name", health)
    must("sticky-force-empty-residual", "AC2 advisory", health)
    must("residual_force_mask", "AC2 health snap field", health)
    must("kStickyForceEmptyResidualIssue", "AC2 issue constant", health)
    must("AC3814", "AC2 throttle test", thr_test)
    must("RequireAgentRepromote", "AC2 throttle test action", thr_test)

    # AC3 Soft/Off zero extra
    must("Soft: zero extra", "AC3 Soft test cite", heal_test)
    must("no silent wholesale clear under Soft", "AC3 Soft clear forbid", heal_test)

    # AC4 soak / documented leave path
    must("ac3814_sticky_force_empty_residual_heal", "AC4 heal test fn", heal_test)
    must("FallBackJit face", "AC4 observe face", hh)
    must("covered sticky force cleared", "AC4 clear assert", heal_test)

    # AC5 wiring / no invent
    must("check_sticky_force_empty_residual_3814", "AC5 build", build)
    must("cmd_sticky_force_empty_residual_3814", "AC5 build cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3814.cpp").is_file():
        fails.append("AC5: test_issue_3814.cpp present (forbidden per #81967)")
    if _read("docs/design/3814-sticky-force-empty-residual.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3814 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3814 sticky force empty residual — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
