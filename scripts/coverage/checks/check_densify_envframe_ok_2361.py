#!/usr/bin/env python3
"""Issue #2361: densify envframe_ok real per-call check coverage.

  AC1: Soft / no densify → envframe_ok true
  AC2: Ownership scan fail → envframe force_reason + fail total
  AC3: Soft zero extra cost
  AC4: query schema-2361 + densify-envframe-ok
  AC5: Phase 5 + scan + tests + gate

Exit 0 = all ACs satisfied.
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

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    env = _read("src/compiler/evaluator_env.cpp")
    efl = _read("src/core/envframe_lifetime.ixx")
    dcr = _read("src/core/densify_consistency_report.h")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_densify_envframe_ok.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 Soft vacuous
    must("envframe_ok = true", "AC1", emb)
    must("ac1_soft_envframe_ok", "AC1", test)

    # AC2 fail-closed (#2368: scan lives inside force_densify_remap_pairing)
    must("Issue #2361", "AC2", emb)
    must("force_densify_remap_pairing", "AC2", emb)
    must("scan_live_env_frame_refs_after_densify", "AC2", env)
    must("densify_ownership_scan_fail_total", "AC2", efl)
    must("inject_densify_ownership_scan_fail_for_test", "AC2", efl)
    must("force_reason", "AC2", test)
    must("envframe", "AC2", test)
    must("ac2_ownership_fail_envframe", "AC2", test)
    must("note_last_densify_envframe_ok", "AC2", emb)
    must("bump_densify_consistency_fail_total", "AC2", emb)

    # AC3 soft path
    must("had_moving_densify", "AC3", emb)
    must("ac1_soft_envframe_ok", "AC3", test)

    # AC4 query
    must("schema-2361", "AC4", q)
    must("issue-2361", "AC4", q)
    must("densify-envframe-axis-wired", "AC4", q)
    must("densify-ownership-scan-fail-total", "AC4", q)
    must("last_densify_envframe_ok", "AC4", q)
    must("ac4_query_schema", "AC4", test)
    # Must not force true on query surface
    if "densify_envframe_ok = true" in q:
        fails.append("AC4: query still forces densify_envframe_ok = true")

    # AC5 gate
    must("scan_live_env_frame_refs_after_densify", "AC5", env)
    must("last_densify_envframe_ok", "AC5", dcr)
    must("test_densify_envframe_ok", "AC5", cmake)
    must("check_densify_envframe_ok_2361", "AC5", build)
    must("cmd_densify_envframe_ok_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2361 densify envframe_ok — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
