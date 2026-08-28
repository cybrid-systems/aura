#!/usr/bin/env python3
"""Issue #3342: pure-anon recovery starvation (heal path, not #3323 race).

Overflow already MustDeopt + poisons epoch (#3024/#3323). Residual remount
tick + bg drain ran only on outermost success BoundaryExit, so
failure-dominated / nested / boundary-sparse HF mutate left a permanent
pure-anon native-hole. Production amortizes heal on outermost failure
when pending ≥ pressure thresh or overflow advanced since last drain.
Success BoundaryExit stays primary. Soft / budget=0: no extra work.
steal-complete still does not drain (#2715). No new query key.

Contract (one row per AC):
  AC1  production overflow/pressure MustDeopt eventually remounts or
       pending stays agent-visible (heal runs on fail-exit)
  AC2  success BoundaryExit drain unchanged (primary)
  AC3  Soft / budget=0: helper no-ops
  AC4  steal-complete does not drain; named remount + storm-clear stay
  AC5  extends test_anonymous_residual_stable_id_policy; linter AFTER
       #3323; no test_issue_3342.cpp; no docs/design/; no schema-3342

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    hur = _read("src/compiler/hot_update_registry.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")
    sh = _read("src/compiler/runtime_shared.h")

    must("Issue #3342", "AC1 cite", rt)
    must("aura_pure_anon_maybe_heal_starved", "AC1 helper", rt)
    must("kPureAnonHealFailExitStride", "AC1 stride", rt)
    must("kPureAnonPressurePendingThresh", "AC1 thresh reuse", rt)
    must("ac3342_1_fail_exit_heals_after_overflow", "AC1 test", test)
    must("aura_pure_anon_maybe_heal_starved", "AC1 boundary", mb)

    must("Issue #2928: outermost success BoundaryExit", "AC2 success cite", mb)
    must("else if (outermost && !success)", "AC2 failure else-if", mb)
    must("ac3342_2_success_boundary_still_primary", "AC2 test", test)
    suc = mb.find("Issue #2928: outermost success BoundaryExit")
    fail = mb.find("else if (outermost && !success)", suc if suc >= 0 else 0)
    if suc < 0 or fail < 0 or fail < suc:
        fails.append("AC2: failure heal must come after success drain")
    if fail >= 0 and "aura_pure_anon_maybe_heal_starved" not in mb[fail : fail + 800]:
        fails.append("AC2: failure else-if does not call starved helper")

    must("production_defaults_active()", "AC3 production gate", rt)
    must("ac3342_3_soft_zero_extra", "AC3 test", test)
    body = rt.find('extern "C" void aura_pure_anon_maybe_heal_starved')
    if body < 0:
        fails.append("AC3: helper definition missing")
    elif "production_defaults_active()" not in rt[body : body + 1800]:
        fails.append("AC3: helper not production-gated")

    must("ac3342_4_pressure_rate_limit_and_steal", "AC4 test", test)
    must("maybe_storm_clear_health_pass", "AC4 storm-clear", hur)
    must("aura_sync_remount_named_live_closures", "AC4 named", rt)
    pos = steal.find("aura_evaluator_on_steal_complete")
    if pos < 0:
        fails.append("AC4: steal-complete missing")
    else:
        win = steal[pos : pos + 8000]
        if "aura_pure_anon_bg_remount_drain" in win:
            fails.append("AC4: steal drains pure-anon (#2715)")
        if "aura_pure_anon_maybe_heal_starved" in win:
            fails.append("AC4: steal starved-heal")

    must("check_pure_anon_heal_starvation_3342", "AC5 build.py", build)
    must("ac3342_5_source_and_linter", "AC5 test", test)
    must("aura_pure_anon_maybe_heal_starved", "AC5 shared.h", sh)
    prev = build.find("check_pure_anon_overflow_dispatch_race_3323")
    ours = build.find("check_pure_anon_heal_starvation_3342")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3323")
    if "schema-3342" in rt or "schema-3342" in mb:
        fails.append("AC5: new schema-3342 query key")
    if "g_3342_" in rt or "g_3342_" in mb:
        fails.append("AC5: new g_3342_* counter in production")
    if _read("tests/compiler/test_issue_3342.cpp"):
        fails.append("AC5: test_issue_3342.cpp present (forbidden #81967)")
    if _read("docs/design/3342-pure-anon-heal-starvation.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3342 pure_anon_heal_starvation:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3342 pure_anon_heal_starvation: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
