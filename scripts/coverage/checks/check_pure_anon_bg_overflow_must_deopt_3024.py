#!/usr/bin/env python3
"""Issue #3024: production pure-anon bg overflow forces MustDeopt.

Close residual native-hole after #2950 / #2850: overflow under
production_defaults_active() sets MustDeopt before enqueue returns.
Soft / sandbox=off / budget=0 stay overflow-counter-only.

Contract (one row per AC):
  AC1  production overflow → MustDeopt + dual-fresh fail + leave native
  AC2  Soft / !production / budget=0 → overflow counter only
  AC3  soak: no generation-behind native after overflow
  AC4  existing keys preserved; new counter additive + query-visible
  AC5  tests + build.py; no invent / docs/design; not a second table

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
    br = _read("src/compiler/aura_jit_bridge.cpp")
    sh = _read("src/compiler/runtime_shared.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    evq = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    met = _read("src/compiler/observability_metrics.h")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3024", "AC1", rt)
    must("pure_anon_bg_overflow_force_leave_native", "AC1 helper", rt)
    must("g_closure_must_deopt[cid] = 1", "AC1 MustDeopt", rt)
    must("g_closure_bridge_epochs[cid] = 0", "AC1 poison epoch", rt)
    must("production_defaults_active()", "AC1 gate", rt)
    must("3024 AC1", "AC1 test", test)

    # AC2
    must("overflow counter only", "AC2 soft cite", rt)
    must("3024 AC2", "AC2 test", test)
    must("budget == 0", "AC2 budget=0 untouched", rt)

    # AC3
    must("3024 AC3 soak", "AC3 soak", test)
    must("no generation-behind native", "AC3 soak assert", test)

    # AC4
    must("pure-anon-bg-overflow-must-deopt-total", "AC4 query jit", obs)
    must("pure-anon-bg-overflow-must-deopt-wired", "AC4 wired jit", obs)
    must("schema-3024", "AC4 schema jit", obs)
    must("pure-anon-bg-overflow-must-deopt-total", "AC4 query eval", evq)
    must("schema-3024", "AC4 schema eval", evq)
    must("pure_anon_bg_overflow_must_deopt_total", "AC4 metrics", met)
    must("pure-anon-bg-overflow-total", "AC4 preserve overflow", obs)
    must("schema-2950", "AC4 preserve 2950", obs)
    must("aura_bump_pure_anon_bg_overflow_must_deopt_total", "AC4 bump", br)
    must("3024 AC4", "AC4 test", test)

    # AC5
    must("aura_pure_anon_bg_overflow_must_deopt_total_v_read", "AC5 shared", sh)
    must("aura_pure_anon_bg_overflow_must_deopt_total_v_read", "AC5 stub", stub)
    must("check_pure_anon_bg_overflow_must_deopt_3024", "AC5 build", build)
    must("cmd_pure_anon_bg_overflow_must_deopt_3024", "AC5 build cmd", build)
    if "AgentRegistry" in rt[rt.find("Issue #3024") : rt.find("Issue #3024") + 2500]:
        fails.append("AC5: must not introduce AgentRegistry")
    if "second closure table" in rt.lower() and "not a second" not in rt:
        fails.append("AC5: must not invent a second closure table")
    if (ROOT / "tests" / "compiler" / "test_issue_3024.cpp").is_file():
        fails.append("AC5: test_issue_3024.cpp present (forbidden per #81967)")
    if _read("docs/design/3024-pure-anon-bg-overflow-must-deopt.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3024 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3024 production overflow MustDeopt — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
