#!/usr/bin/env python3
"""Issue #2365: RootRemap + densify Closure/EnvFrame dual-epoch closed-loop.

  AC1: Soft vacuous root_remap + closure_remount
  AC2: last-call RootRemap fail inject + force_reason
  AC3: dual-epoch revalidate + order documented
  AC4: query schema-2365 + last-axis publish
  AC5: Phase 5 wire + tests + gate

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
    dcr = _read("src/core/densify_consistency_report.h")
    rrp = _read("src/compiler/root_remap_pass.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_densify_root_closure_closed_loop_2365.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 Soft vacuous
    must("root_remap_ok = true", "AC1", emb)
    must("closure_remount_ok = true", "AC1", emb)
    must("ac1_soft_vacuous", "AC1", test)

    # AC2 last-call RootRemap
    must("last_root_remap_any_fail", "AC2", emb)
    must("inject_last_root_remap_any_fail_for_test", "AC2", rrp)
    must("note_last_densify_root_remap_ok", "AC2", emb)
    must("ac2_root_remap_fail", "AC2", test)

    # AC3 dual-epoch + order (#2368 folds remount/dual into force_densify_remap_pairing)
    must("force_densify_remap_pairing", "AC3", emb)
    must("revalidate_dual_epoch_after_densify", "AC3", env)
    must("densify-success closed-loop order", "AC3", dcr)
    must("Issue #2365", "AC3", dcr)
    must("scan_live_closures_for_linear_captures", "AC3", env)
    must("ac3_dual_epoch_closure", "AC3", test)

    # AC4 query
    must("schema-2365", "AC4", q)
    must("issue-2365", "AC4", q)
    must("densify-root-remap-axis-wired", "AC4", q)
    must("densify-closure-remount-axis-wired", "AC4", q)
    must("densify-dual-epoch-closed-loop-wired", "AC4", q)
    must("last_densify_root_remap_ok", "AC4", q)
    must("last_densify_closure_remount_ok", "AC4", q)
    must("ac4_query", "AC4", test)

    # AC5 gate
    must("Issue #2365", "AC5", emb)
    must("test_densify_root_closure_closed_loop_2365", "AC5", cmake)
    must("check_densify_root_closure_closed_loop_2365", "AC5", build)
    must("cmd_densify_root_closure_closed_loop_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2365 densify RootRemap+closure dual-epoch — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
