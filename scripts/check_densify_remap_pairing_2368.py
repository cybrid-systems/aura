#!/usr/bin/env python3
"""Issue #2368: force densify remap-context pairing on Moving success paths.

  AC1: Soft vacuous / pairing not forced
  AC2: negative RootRemap inject → !overall_ok
  AC3: permanent order EnvFrame → remount → dual-epoch (force helper)
  AC4: query schema-2368 + pairing-forced / dual-epoch keys
  AC5: Phase 5 uses force_densify_remap_pairing + gate

Exit 0 = all ACs satisfied.
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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    env = _read("src/compiler/evaluator_env.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    dcr = _read("src/core/densify_consistency_report.h")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_densify_remap_pairing_2368.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 Soft
    must("note_last_densify_remap_pairing_forced(false)", "AC1", emb)
    must("ac1_soft_vacuous", "AC1", test)

    # AC2 negative
    must("last_root_remap_any_fail", "AC2", env)
    must("ac2_missed_remap_negative", "AC2", test)
    must("inject_last_root_remap_any_fail_for_test", "AC2", test)

    # AC3 order
    must("force_densify_remap_pairing", "AC3", env)
    must("force_densify_remap_pairing", "AC3", ixx)
    must("Issue #2368", "AC3", dcr)
    must("never optional", "AC3", dcr)
    must("scan_live_env_frame_refs_after_densify()", "AC3", env)
    must("scan_live_closures_for_linear_captures", "AC3", env)
    must("revalidate_dual_epoch_after_densify()", "AC3", env)
    # Order inside force helper: envframe before remount before dual
    force_at = env.find("Evaluator::force_densify_remap_pairing")
    if force_at < 0:
        fails.append("AC3: force method body missing")
    else:
        body = env[force_at : force_at + 2500]
        p_e = body.find("scan_live_env_frame_refs_after_densify()")
        p_c = body.find("scan_live_closures_for_linear_captures")
        p_d = body.find("revalidate_dual_epoch_after_densify()")
        if not (0 <= p_e < p_c < p_d):
            fails.append(f"AC3: order not EnvFrame→remount→dual (e={p_e} c={p_c} d={p_d})")
    must("ac3_pairing_order_and_dual", "AC3", test)

    # AC4 query
    must("schema-2368", "AC4", q)
    must("issue-2368", "AC4", q)
    must("densify-remap-pairing-forced", "AC4", q)
    must("densify-dual-epoch-ok", "AC4", q)
    must("densify-remap-pairing-forced-wired", "AC4", q)
    must("ac4_query", "AC4", test)

    # AC5 gate + Phase 5
    must("force_densify_remap_pairing", "AC5", emb)
    must("Issue #2368", "AC5", emb)
    must("test_densify_remap_pairing_2368", "AC5", cmake)
    must("check_densify_remap_pairing_2368", "AC5", build)
    must("cmd_densify_remap_pairing_coverage", "AC5", build)
    must("ac5_phase5_source", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2368 densify remap pairing — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
