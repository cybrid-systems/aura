#!/usr/bin/env python3
"""Issue #2376: DensifyConsistencyReport per-call last-result (envframe + closure).

Seals #2361/#2365 last-call contract as production densify-consistency:
  AC1 envframe last-call fail (not force-true / not only cumulative)
  AC2 closure remount last-call fail delta
  AC3 Soft vacuous true
  AC4 query schema-2376 + last-call-seq + fail codes
  AC5 Phase 5 + pairing + gate

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    env = _read("src/compiler/evaluator_env.cpp")
    dcr = _read("src/core/densify_consistency_report.h")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_densify_last_call_axes_2376.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 envframe last-call
    must("note_last_densify_envframe_ok", "AC1", emb)
    must("kDensifyEnvframeFailOwnershipScan", "AC1", dcr)
    must("Issue #2376", "AC1", emb)
    must("ac1_envframe_last_call_fail", "AC1", test)

    # AC2 closure last-call
    must("note_last_densify_closure_remount_ok", "AC2", emb)
    must("cl_fail0", "AC2", env)
    must("cl_fail1", "AC2", env)
    must("kDensifyClosureFailCaptureRemap", "AC2", dcr)
    must("ac2_closure_last_call_fail", "AC2", test)

    # AC3 Soft
    must("envframe_ok = true", "AC3", emb)
    must("closure_remount_ok = true", "AC3", emb)
    must("ac3_soft_vacuous", "AC3", test)

    # AC4 query
    must("schema-2376", "AC4", q)
    must("issue-2376", "AC4", q)
    must("densify-last-call-seq", "AC4", q)
    must("densify-last-call-axes-wired", "AC4", q)
    must("densify-envframe-fail-code", "AC4", q)
    must("densify-closure-fail-code", "AC4", q)
    must("last_densify_envframe_ok", "AC4", q)
    must("last_densify_closure_remount_ok", "AC4", q)
    must("ac4_query", "AC4", test)
    if "densify_envframe_ok = true" in q:
        fails.append("AC4: query still forces densify_envframe_ok = true")

    # AC5 gate + pairing
    must("bump_last_densify_call_seq", "AC5", emb)
    must("Issue #2376", "AC5", env)
    must("force_densify_remap_pairing", "AC5", emb)
    must("AURA_DENSIFY_CONTRACT", "AC5", emb)
    must("g_last_densify_call_seq", "AC5", dcr)
    must("test_densify_last_call_axes_2376", "AC5", cmake)
    must("check_densify_last_call_axes_2376", "AC5", build)
    must("cmd_densify_last_call_axes_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2376 densify last-call axes — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
