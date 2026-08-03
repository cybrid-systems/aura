#!/usr/bin/env python3
"""Issue #2605: explicit anonymous / residual sid=0 policy.

Contract:
  AC1 residual named sid=0 backfill + named invent reject bump present
  AC2 anonymous MustDeopt path retained; no silent name invent for production
  AC3 AURA_NAMED_NAME_FALLBACK_HARD hard invent env
  AC4 schema-2605 + assign/preserve/residual_backfill query axes
  AC5 test + cmake + build.py gate

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    bh = _read("src/compiler/aura_jit_bridge.h")
    bc = _read("src/compiler/aura_jit_bridge.cpp")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qq = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy_2605.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 residual backfill + named invent
    must("Issue #2605", "AC1", rt)
    must("aura_bump_live_closure_stable_id_backfill_total", "AC1", rt)
    must("aura_test_force_closure_stable_func_id", "AC1", rt + bh)
    must("aura_bump_live_closure_named_name_fallback_reject_total", "AC1", rt)
    must("live_closure_named_name_fallback_reject_total", "AC1", met)
    must("aura_bump_live_closure_named_name_fallback_reject_total", "AC1", bh)
    must("aura_bump_live_closure_named_name_fallback_reject_total", "AC1", bc + stub)

    # AC2 anonymous MustDeopt
    must("anonymous / residual sid=0", "AC2", rt)
    must("MustDeopt", "AC2", rt)

    # AC3 hard invent env
    must("AURA_NAMED_NAME_FALLBACK_HARD", "AC3", rt)

    # AC4 query axes
    must("schema-2605", "AC4", q + qq)
    must("issue-2605", "AC4", q + qq)
    must("stable-id-residual-backfill-total", "AC4", q + qq)
    must("stable-id-assign-total", "AC4", q + qq)
    must("stable-id-preserve-total", "AC4", q + qq)
    must("named-name-fallback-reject-total", "AC4", q + qq)
    must("residual-sid0-policy-wired", "AC4", q + qq)
    must("anonymous-must-deopt-policy-wired", "AC4", q + qq)

    # AC5 wiring
    must("ac1_named_soak_no_residual_growth", "AC5", test)
    must("ac3_residual_one_shot_backfill", "AC5", test)
    must("test_anonymous_residual_stable_id_policy_2605", "AC5", cmake)
    must("check_anonymous_residual_stable_id_policy_2605", "AC5", build)
    must("cmd_anonymous_residual_stable_id_policy_coverage", "AC5", build)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2605 anonymous/residual sid=0 policy — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
