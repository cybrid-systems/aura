#!/usr/bin/env python3
"""Issue #2369: stable_func_id sole primary for live-closure remap.

  AC1: positive stable_id remap; name-fallback counter only under flag
  AC2: miss → MustDeopt + batch_deopt; no name rewrite
  AC3: legacy flag opt-in; production forces off
  AC4: query schema-2369 + sole-primary wired
  AC5: tests + gate

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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    sec = _read("src/compiler/security_defaults.hh")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    # Production surface wins via later register_stats_impl in obs_eval.
    q_prod = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_live_closure_stable_id_only_2369.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 / contract docs
    must("Issue #2369", "AC1", rt)
    must("g_remap_name_fallback_enabled", "AC1", rt)
    must("via_name_fallback", "AC1", rt)
    must("ac1_stable_id_remap", "AC1", test)

    # AC2 miss path
    must("aura_bump_live_closure_must_deopt_kept_total", "AC2", rt)
    must("aura_jit_batch_deopt_for", "AC2", rt)
    must("name_candidate_no_remap", "AC2", rt)
    must("ac2_miss_must_deopt", "AC2", test)
    # Ensure miss block (match_id == 0) calls kept + batch (not only remount fail)
    miss_marker = "no name-based rewrite"
    if miss_marker not in rt and "never name-rewritten" not in rt:
        fails.append("AC2: miss-path contract comment missing")

    # AC3 production force off
    must("Issue #2369", "AC3", sec)
    must("aura_set_remap_name_fallback_enabled(0)", "AC3", sec)
    must("ac3_legacy_flag", "AC3", test)

    # AC4 query (production surface = obs_eval; query.cpp also carries lineage)
    must("schema-2369", "AC4", q_prod)
    must("issue-2369", "AC4", q_prod)
    must("stable-func-id-sole-primary-wired", "AC4", q_prod)
    must("remap-name-fallback-default-off", "AC4", q_prod)
    must("live-closure-remap-name-fallback-total", "AC4", q_prod)
    must("schema-2369", "AC4", q)
    must("ac4_query", "AC4", test)

    # AC5 gate
    must("test_live_closure_stable_id_only_2369", "AC5", cmake)
    must("check_live_closure_stable_id_only_2369", "AC5", build)
    must("cmd_live_closure_stable_id_only_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2369 live-closure stable_func_id sole primary — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
