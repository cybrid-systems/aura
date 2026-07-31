#!/usr/bin/env python3
"""Issue #2362: EnvFrameRef ownership under fiber steal + densify.

Depends on #2360 live set. AC rows:

  AC1: live set register/inject + materialize auto-register + Soft empty free
  AC2: gen advanced → transfer_to; OOB → drop; metrics only on real events
  AC3: fiber steal path wires sync_live_env_frame_refs_ownership
  AC4: densify scan real protocol (scan + transfer/drop)
  AC5: query schema-2362 + hold-pin retained + tests/gate

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

    env = _read("src/compiler/evaluator_env.cpp")
    eval_ixx = _read("src/compiler/evaluator.ixx")
    mut = _read("src/compiler/evaluator_fiber_mutation.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    q = _read("src/compiler/evaluator_primitives_mutate.cpp")
    lf = _read("src/core/envframe_lifetime.ixx")
    test = _read("tests/compiler/test_envframe_ownership_steal_densify_2362.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 live set
    must("register_live_env_frame_ref", "AC1", eval_ixx)
    must("inject_live_env_frame_ref_for_test", "AC1", env)
    must("live_env_frame_ref_slots_", "AC1", eval_ixx)
    must("register_live_env_frame_ref(ref)", "AC1", env)
    must("ac1_live_set", "AC1", test)
    # materialize auto-register
    must("register_live_env_frame_ref(ref)", "AC1", env)

    # AC2 protocol
    must("sync_live_env_frame_refs_ownership", "AC2", eval_ixx)
    must("sync_live_env_frame_refs_ownership", "AC2", env)
    must("transfer_to(*this, restamped)", "AC2", env)
    must("ac2_transfer_drop_protocol", "AC2", test)

    # AC3 steal
    must("Issue #2362", "AC3", mut)
    must("sync_live_env_frame_refs_ownership", "AC3", mut)
    must("ac3_steal_wire", "AC3", test)

    # AC4 densify (#2368: Phase 5 calls force_densify_remap_pairing which
    # owns scan_live_env_frame_refs_after_densify)
    must("scan_live_env_frame_refs_after_densify", "AC4", env)
    must("sync_live_env_frame_refs_ownership", "AC4", env)
    must("force_densify_remap_pairing", "AC4", emb)
    must("ac4_densify_scan", "AC4", test)

    # AC5 query + hold-pin + gate
    must("schema-2362", "AC5", q)
    must("issue-2362", "AC5", q)
    must("schema-2360", "AC5", q)
    must("live-env-frame-refs-wired", "AC5", q)
    must("live-env-frame-refs-count", "AC5", q)
    must("envframe-ownership-protocol-steal-wired", "AC5", q)
    must("envframe-ownership-protocol-densify-wired", "AC5", q)
    must("should_block_compact_for_guards", "AC5", lf)
    must("test_envframe_ownership_steal_densify_2362", "AC5", cmake)
    must("check_envframe_ownership_steal_densify_2362", "AC5", build)
    must("cmd_envframe_ownership_steal_densify_coverage", "AC5", build)
    must("ac5_query_and_surface", "AC5", test)

    # Must not leave densify scan as empty no-op stub
    if "Production logic (follow-up)" in env and "for now: no live refs" in env.lower():
        fails.append("AC4: densify scan still a no-op stub")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2362 EnvFrameRef ownership steal+densify — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
